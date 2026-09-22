@EndUserText.label : 'erpl-rev APE volume fixture (100k source, no CDC)'
@AbapCatalog.enhancement.category : #NOT_EXTENSIBLE
@AbapCatalog.tableCategory : #TRANSPARENT
@AbapCatalog.deliveryClass : #A
@AbapCatalog.dataMaintenance : #RESTRICTED
define table zerpl_ape_v {

  key rid   : abap.char(10) not null;
  descr     : abap.char(40);
  amount    : abap.dec(13,2);
  waers     : abap.char(5);
  qty       : abap.int4;
  rev       : abap.int4;

}
