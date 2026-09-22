@AbapCatalog.viewEnhancementCategory: [#NONE]
@AccessControl.authorizationCheck: #NOT_REQUIRED
@EndUserText.label: 'ERPL APE volume source (extraction-enabled, no capture)'
@Analytics.dataExtraction.enabled: true
define view entity ZERPL_APE_VOL
  as select from zerpl_ape_v
{
  key Rid    as Rid,
      Descr  as Descr,
      Amount as Amount,
      Waers  as Waers,
      Qty    as Qty,
      Rev    as Rev
}
