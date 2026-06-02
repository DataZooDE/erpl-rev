// erpl-rev self-extracting launcher (Option 1).
//
// The distributable single binary = this launcher with a payload appended:
//   [ launcher executable ][ payload.tar (inner server + runtime libs) ][ footer ]
// footer = 8-byte magic "ERPLREV\x01" + little-endian uint64 payload size.
//
// On start the launcher locates its own executable, reads the footer, extracts
// the ustar payload to a per-version cache dir under the system temp directory,
// points the dynamic loader at that dir, and runs the inner erpl_rev_server —
// so a user ships exactly one file and needs no SAP NW RFC SDK / DuckDB on disk.
//
// Cross-platform (Linux / macOS / Windows): no objcopy/resource tricks — the
// payload is plain appended bytes, so the same code compiles everywhere.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#else
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#endif
#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif

namespace {

const char kMagic[8] = {'E', 'R', 'P', 'L', 'R', 'E', 'V', '\x01'};
constexpr uint64_t kFooterSize = 16;  // 8 magic + 8 LE size

#if defined(_WIN32)
const char kInnerName[] = "erpl_rev_server.exe";
using PathChar = wchar_t;
#else
const char kInnerName[] = "erpl_rev_server";
#endif

[[noreturn]] void die(const std::string &msg) {
  std::fprintf(stderr, "erpl-rev launcher: %s\n", msg.c_str());
  std::exit(70);
}

// --- locate our own executable -------------------------------------------
std::string SelfPath() {
#if defined(_WIN32)
  std::vector<wchar_t> buf(32768);
  DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
  if (n == 0) die("GetModuleFileNameW failed");
  int len = WideCharToMultiByte(CP_UTF8, 0, buf.data(), n, nullptr, 0, nullptr, nullptr);
  std::string out(len, '\0');
  WideCharToMultiByte(CP_UTF8, 0, buf.data(), n, out.data(), len, nullptr, nullptr);
  return out;
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buf(size);
  if (_NSGetExecutablePath(buf.data(), &size) != 0) die("_NSGetExecutablePath failed");
  char real[4096];
  if (realpath(buf.data(), real)) return std::string(real);
  return std::string(buf.data());
#else
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) die("readlink /proc/self/exe failed");
  buf[n] = '\0';
  return std::string(buf);
#endif
}

uint64_t FileSize(std::FILE *f) {
#if defined(_WIN32)
  _fseeki64(f, 0, SEEK_END);
  long long s = _ftelli64(f);
#else
  std::fseek(f, 0, SEEK_END);
  long s = std::ftell(f);
#endif
  return (uint64_t)s;
}

void Seek(std::FILE *f, uint64_t off) {
#if defined(_WIN32)
  _fseeki64(f, (long long)off, SEEK_SET);
#else
  std::fseek(f, (long)off, SEEK_SET);
#endif
}

// --- filesystem helpers ---------------------------------------------------
bool Exists(const std::string &p) {
#if defined(_WIN32)
  return _access(p.c_str(), 0) == 0;
#else
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
#endif
}

void MakeDir(const std::string &p) {
#if defined(_WIN32)
  _mkdir(p.c_str());
#else
  ::mkdir(p.c_str(), 0700);
#endif
}

std::string TempRoot() {
#if defined(_WIN32)
  char buf[MAX_PATH];
  DWORD n = GetTempPathA(MAX_PATH, buf);
  if (n == 0) return ".";
  return std::string(buf, n);
#else
  const char *t = std::getenv("TMPDIR");
  return t && *t ? std::string(t) : std::string("/tmp");
#endif
}

char Sep() {
#if defined(_WIN32)
  return '\\';
#else
  return '/';
#endif
}

std::string Join(const std::string &a, const std::string &b) {
  if (a.empty()) return b;
  if (a.back() == Sep()) return a + b;
  return a + Sep() + b;
}

// --- minimal ustar reader -------------------------------------------------
uint64_t ParseOctal(const char *p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n && p[i] >= '0' && p[i] <= '7'; ++i) v = v * 8 + (p[i] - '0');
  return v;
}

// Extract the ustar stream in [start, start+size) of file f into dir.
void ExtractTar(std::FILE *f, uint64_t start, uint64_t size, const std::string &dir) {
  Seek(f, start);
  uint64_t consumed = 0;
  std::vector<char> hdr(512);
  while (consumed + 512 <= size) {
    if (std::fread(hdr.data(), 1, 512, f) != 512) break;
    consumed += 512;
    bool zero = true;
    for (char c : hdr) if (c) { zero = false; break; }
    if (zero) break;  // end-of-archive

    std::string name(hdr.data(), strnlen(hdr.data(), 100));
    uint64_t fsize = ParseOctal(hdr.data() + 124, 12);
    char type = hdr[156];
    // strip leading "./"
    if (name.rfind("./", 0) == 0) name = name.substr(2);

    if (type == '5' || name.empty()) {  // directory / pax — skip data
      uint64_t skip = (fsize + 511) & ~uint64_t(511);
      Seek(f, start + consumed + skip);
      consumed += skip;
      continue;
    }

    std::string out = Join(dir, name);
    std::FILE *o = std::fopen(out.c_str(), "wb");
    if (!o) die("cannot write " + out);
    uint64_t left = fsize;
    std::vector<char> buf(1 << 20);
    while (left > 0) {
      size_t want = (size_t)(left < buf.size() ? left : buf.size());
      size_t got = std::fread(buf.data(), 1, want, f);
      if (got == 0) break;
      std::fwrite(buf.data(), 1, got, o);
      left -= got;
    }
    std::fclose(o);
    uint64_t padded = (fsize + 511) & ~uint64_t(511);
    consumed += padded;
    Seek(f, start + consumed);
#if !defined(_WIN32)
    if (name == kInnerName) ::chmod(out.c_str(), 0755);
#endif
  }
}

}  // namespace

int main(int argc, char **argv) {
  std::string self = SelfPath();
  std::FILE *f = std::fopen(self.c_str(), "rb");
  if (!f) die("cannot open self: " + self);

  uint64_t total = FileSize(f);
  if (total < kFooterSize) die("bundle too small / no payload");

  // read footer
  Seek(f, total - kFooterSize);
  char footer[16];
  if (std::fread(footer, 1, 16, f) != 16) die("cannot read footer");
  if (std::memcmp(footer, kMagic, 8) != 0) die("bad payload magic (not a bundled binary)");
  uint64_t payload_size = 0;
  for (int i = 0; i < 8; ++i) payload_size |= (uint64_t)(unsigned char)footer[8 + i] << (8 * i);
  if (payload_size + kFooterSize > total) die("payload size out of range");
  uint64_t payload_start = total - kFooterSize - payload_size;

  // per-version cache dir keyed on payload size (cheap version id)
  char idbuf[32];
  std::snprintf(idbuf, sizeof(idbuf), "erpl-rev-%llx", (unsigned long long)payload_size);
  std::string cache = Join(TempRoot(), idbuf);
  std::string ready = Join(cache, ".ready");
  std::string inner = Join(cache, kInnerName);

  if (!Exists(ready)) {
    MakeDir(cache);
    ExtractTar(f, payload_start, payload_size, cache);
    std::FILE *r = std::fopen(ready.c_str(), "wb");
    if (r) std::fclose(r);
  }
  std::fclose(f);
  if (!Exists(inner)) die("inner server missing after extraction");

  // point the dynamic loader at the cache dir and run the inner server
#if defined(_WIN32)
  // DLLs sit beside the inner exe in `cache`, which Windows searches by default;
  // also prepend to PATH for robustness, then CreateProcess.
  {
    std::string path = "PATH=" + cache;
    if (const char *old = std::getenv("PATH")) { path += ";"; path += old; }
    _putenv(path.c_str());
  }
  std::wstring wcache(cache.begin(), cache.end());
  std::wstring winner(inner.begin(), inner.end());
  // rebuild a command line: "inner" + original args (skip our argv[0])
  std::wstring cmd = L"\"" + winner + L"\"";
  int wargc = 0;
  LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  for (int i = 1; i < wargc; ++i) { cmd += L" \""; cmd += wargv[i]; cmd += L"\""; }
  STARTUPINFOW si{};  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());  cmdbuf.push_back(0);
  if (!CreateProcessW(winner.c_str(), cmdbuf.data(), nullptr, nullptr, TRUE,
                      0, nullptr, wcache.c_str(), &si, &pi))
    die("CreateProcess failed");
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  return (int)code;
#else
  {
#  if defined(__APPLE__)
    const char *var = "DYLD_LIBRARY_PATH";
#  else
    const char *var = "LD_LIBRARY_PATH";
#  endif
    std::string val = cache;
    if (const char *old = std::getenv(var)) { val += ":"; val += old; }
    setenv(var, val.c_str(), 1);
#  if defined(__APPLE__)
    setenv("DYLD_FALLBACK_LIBRARY_PATH", cache.c_str(), 1);
#  endif
  }
  std::vector<char *> args;
  args.push_back(const_cast<char *>(inner.c_str()));
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
  args.push_back(nullptr);
  execv(inner.c_str(), args.data());
  die(std::string("execv failed: ") + std::strerror(errno));
#endif
}
