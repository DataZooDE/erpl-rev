* erpl-rev: one column per replication strategy, on a BSEG-shaped row.
*
* Shaped after BSEG/BKPF -- a line-item table with a compound key -- so a
* strategy proven here is proven against something the shape of what customers
* actually replicate, not against a two-column toy.
*
* It carries ONE column per delta strategy on purpose. Real SAP tables rarely
* offer more than one, so exercising each strategy normally means a different
* table AND a different workload, and the results are then not comparable. Here
* the same rows, the same generator and the same key can be replicated five ways
* and the numbers put side by side.
*
*   chg_tstamp            NUMTS / TIMESTAMPL  sub-second high-water
*   chg_dats              DATE                the complete-day rule
*   chg_date2 + chg_time  DATETIME            the DATS+TIMS pair
*   chg_counter           INT                 a monotonic counter
*   (no column needed)    SNAPSHOT            anti-join
*   (no column needed)    trigger CDC         transparent, so triggers work
*
* CHANGEDOC and INSERT_ONLY drive off CDHDR rather than a column here; the
* change-injection driver writes synthetic change documents for those.
*
* Amounts are DEC rather than CURR/QUAN deliberately: those require a reference
* field for their currency/unit, which is DDIC machinery unrelated to what is
* being tested.
*
* NOTE: the lines above are stripped before deployment. SAP's DDL parser rejects
* comments inside the table body, which is why they live here as a header.
@EndUserText.label : 'erpl-rev: one column per replication strategy'
@AbapCatalog.enhancement.category : #NOT_EXTENSIBLE
@AbapCatalog.tableCategory : #TRANSPARENT
@AbapCatalog.deliveryClass : #A
@AbapCatalog.dataMaintenance : #RESTRICTED
define table zdelta_all {

  key client      : abap.clnt not null;
  key bukrs       : abap.char(4) not null;
  key belnr       : abap.char(10) not null;
  key gjahr       : abap.numc(4) not null;
  key buzei       : abap.numc(3) not null;

  chg_tstamp      : abap.dec(21,7);
  chg_dats        : abap.dats;
  chg_date2       : abap.dats;
  chg_time        : abap.tims;
  chg_counter     : abap.int8;

  bschl           : abap.char(2);
  koart           : abap.char(1);
  shkzg           : abap.char(1);
  dmbtr           : abap.dec(23,2);
  wrbtr           : abap.dec(23,2);
  pswbt           : abap.dec(23,2);
  waers           : abap.cuky(5);
  hkont           : abap.char(10);
  kunnr           : abap.char(10);
  lifnr           : abap.char(10);
  kostl           : abap.char(10);
  aufnr           : abap.char(12);
  matnr           : abap.char(40);
  werks           : abap.char(4);
  menge           : abap.dec(13,3);
  meins           : abap.unit(3);
  zuonr           : abap.char(18);
  sgtxt           : abap.char(50);
  xnegp           : abap.char(1);
  budat           : abap.dats;
  bldat           : abap.dats;

}
