@EndUserText.label : 'erpl-rev DATE watermark test table'
@AbapCatalog.enhancement.category : #NOT_EXTENSIBLE
@AbapCatalog.tableCategory : #TRANSPARENT
@AbapCatalog.deliveryClass : #A
@AbapCatalog.dataMaintenance : #RESTRICTED
define table zdelta_d {

  key client     : abap.clnt not null;
  key id         : abap.char(10) not null;
  name           : abap.char(40);
  changed_on     : abap.dats;

}
