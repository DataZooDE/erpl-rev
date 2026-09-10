* Goods movements, MSEG-shaped: material document line items.
*
* The stock counterpart to ZDELTA_ALL's accounting document. Key and field
* names follow MSEG so the shape is recognisable to anyone who has worked with
* SAP logistics -- MBLNR/MJAHR/ZEILE, movement type in BWART, material and
* plant and storage location, quantity in MENGE against MEINS.
*
* Deliberately a Z table, not MSEG itself: A4H is a bare ABAP Platform trial
* with no Materials Management, so there is no real goods movement to
* replicate. The shape is faithful; the provenance is not, and anything built
* on it must say so.
*
* Amounts and quantities are DEC rather than CURR/QUAN for the same reason
* ZDELTA_ALL gives: those need a reference field for their currency or unit,
* which is DDIC machinery unrelated to what is being tested here.
*
* It carries the same five change columns ZDELTA_ALL does, so any of the
* replication strategies can drive it -- watermark on CHG_TSTAMP, the DATS plus
* TIMS pair, a counter -- even though the demo runs it on the trigger tier.
*
* NOTE: the lines above are stripped before deployment. SAP's DDL parser
* rejects comments inside the table body, which is why they live here as a
* header.
@EndUserText.label : 'erpl-rev goods movements (MSEG-shaped, test only)'
@AbapCatalog.enhancement.category : #NOT_EXTENSIBLE
@AbapCatalog.tableCategory : #TRANSPARENT
@AbapCatalog.deliveryClass : #A
@AbapCatalog.dataMaintenance : #RESTRICTED
define table zstock_move {

  key client     : abap.clnt not null;
  key mblnr      : abap.char(10) not null;
  key mjahr      : abap.numc(4) not null;
  key zeile      : abap.numc(4) not null;

  chg_tstamp     : abap.dec(21,7);
  chg_dats       : abap.dats;
  chg_date2      : abap.dats;
  chg_time       : abap.tims;
  chg_counter    : abap.int8;

  bwart          : abap.char(3);
  matnr          : abap.char(40);
  werks          : abap.char(4);
  lgort          : abap.char(4);
  charg          : abap.char(10);
  bukrs          : abap.char(4);
  menge          : abap.dec(13,3);
  meins          : abap.unit(3);
  dmbtr          : abap.dec(23,2);
  waers          : abap.cuky(5);
  lifnr          : abap.char(10);
  kostl          : abap.char(10);
  budat          : abap.dats;
  bldat          : abap.dats;
  sgtxt          : abap.char(50);

}
