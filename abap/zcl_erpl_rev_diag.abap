CLASS zcl_erpl_rev_diag DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.
CLASS zcl_erpl_rev_diag IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    DATA: lv_req TYPE c LENGTH 255 VALUE 'hi',
          lv_echo TYPE c LENGTH 255, lv_resp TYPE c LENGTH 255,
          lv_msg TYPE c LENGTH 255.
    CALL FUNCTION 'STFC_CONNECTION' DESTINATION 'ERPL_REV'
      EXPORTING requtext = lv_req
      IMPORTING echotext = lv_echo resptext = lv_resp
      EXCEPTIONS system_failure = 1 MESSAGE lv_msg
                 communication_failure = 2 MESSAGE lv_msg
                 OTHERS = 3.
    out->write( |subrc={ sy-subrc }| ).
    out->write( |msg=[{ lv_msg }]| ).
    out->write( |msgid={ sy-msgid } msgno={ sy-msgno }| ).
    out->write( |v1=[{ sy-msgv1 }] v2=[{ sy-msgv2 }]| ).
    out->write( |echo=[{ lv_echo }] resp=[{ lv_resp }]| ).

    " Can the calling user create and activate a class? `erpl-rev setup`
    " deploys ABAP over ADT, and the CLI's sync/replicate commands generate a
    " throwaway class to carry their parameters -- both need S_DEVELOP. A
    " production RFC service user usually has none, and without this check the
    " first sign of that is an opaque object-create failure mid-deploy.
    " AUTHORITY-CHECK reads; it changes nothing, so doctor may run it.
    AUTHORITY-CHECK OBJECT 'S_DEVELOP'
      ID 'DEVCLASS' DUMMY
      ID 'OBJTYPE'  FIELD 'CLAS'
      ID 'OBJNAME'  DUMMY
      ID 'P_GROUP'  DUMMY
      ID 'ACTVT'    FIELD '01'.
    DATA(lv_create) = sy-subrc.
    AUTHORITY-CHECK OBJECT 'S_DEVELOP'
      ID 'DEVCLASS' DUMMY
      ID 'OBJTYPE'  FIELD 'CLAS'
      ID 'OBJNAME'  DUMMY
      ID 'P_GROUP'  DUMMY
      ID 'ACTVT'    FIELD '02'.
    out->write( |s_develop create={ lv_create } change={ sy-subrc } user={ sy-uname }| ).
  ENDMETHOD.
ENDCLASS.
