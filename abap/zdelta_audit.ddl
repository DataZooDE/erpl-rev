@EndUserText.label : 'erpl-rev change-generator audit (test only)'
@AbapCatalog.enhancement.category : #NOT_EXTENSIBLE
@AbapCatalog.tableCategory : #TRANSPARENT
@AbapCatalog.deliveryClass : #A
@AbapCatalog.dataMaintenance : #RESTRICTED
define table zdelta_audit {

  key client     : abap.clnt not null;
  key seqno      : abap.int4 not null;
  runid          : abap.char(20);
  keyval         : abap.char(60);
  op             : abap.char(1);
  committed_at   : abap.dec(21,7);

}
