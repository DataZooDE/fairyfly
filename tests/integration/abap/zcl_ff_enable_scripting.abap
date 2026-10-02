CLASS zcl_ff_enable_scripting DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.

CLASS zcl_ff_enable_scripting IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    DATA lv_value TYPE spfl_parameter_value.
    DATA lv_msg   TYPE string.
    DATA lv_rc    TYPE i.

    cl_spfl_profile_parameter=>get_value( EXPORTING name = 'sapgui/user_scripting'
                                          IMPORTING value = lv_value
                                          RECEIVING rc = lv_rc ).
    out->write( |before: sapgui/user_scripting = '{ lv_value }' (rc { lv_rc })| ).

    cl_spfl_profile_parameter=>change_value( EXPORTING name = 'sapgui/user_scripting'
                                                       value = 'TRUE'
                                             IMPORTING msg = lv_msg
                                             RECEIVING rc = lv_rc ).
    out->write( |change_value rc { lv_rc }: { lv_msg }| ).

    CLEAR lv_value.
    cl_spfl_profile_parameter=>get_value( EXPORTING name = 'sapgui/user_scripting'
                                          IMPORTING value = lv_value
                                          RECEIVING rc = lv_rc ).
    out->write( |after: sapgui/user_scripting = '{ lv_value }' (rc { lv_rc })| ).
  ENDMETHOD.
ENDCLASS.
