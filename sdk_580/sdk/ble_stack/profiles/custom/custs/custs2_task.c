/**
****************************************************************************************
*
* @file custs2_task.c
*
* @brief Custom Service profile task source file.
*
* Copyright (C) 2014. Dialog Semiconductor Ltd, unpublished work. This computer 
* program includes Confidential, Proprietary Information and is a Trade Secret of 
* Dialog Semiconductor Ltd.  All use, disclosure, and/or reproduction is prohibited 
* unless authorized in writing. All Rights Reserved.
*
* <bluetooth.support@diasemi.com> and contributors.
*
****************************************************************************************
*/
#include "rwble_config.h"              // SW configuration
#if (BLE_CUSTOM2_SERVER)
#include "custs2_task.h"
#include "custs2.h"
#include "custom_common.h"
#include "attm_db_128.h"
#include "ke_task.h"
#include "gapc.h"
#include "gapc_task.h"
#include "gattc_task.h"
#include "attm_db.h"
#include "atts_util.h"
#include "prf_utils.h"
#include "app_prf_types.h"
/*
 * FUNCTION DEFINITIONS
 ****************************************************************************************
 */

/**
 ****************************************************************************************
 * @brief Handles reception of the @ref CUSTS2_CREATE_DB_REQ message.
 * @param[in] msgid Id of the message received (probably unused).
 * @param[in] param Pointer to the parameters of the message.
 * @param[in] dest_id ID of the receiving task instance (probably unused).
 * @param[in] src_id ID of the sending task instance.
 * @return If the message was consumed or not.
 ****************************************************************************************
 */

static int custs2_create_db_req_handler(ke_msg_id_t const msgid,
                                      struct custs2_create_db_req const *param,
                                      ke_task_id_t const dest_id,
                                      ke_task_id_t const src_id)
{
    // Database Creation Status
    uint8_t status;
    // Save Profile ID
    custs2_env.con_info.prf_id = dest_id;
    const struct attm_desc_128 *att_db = NULL;    
    uint8_t i=0;
    
    while( cust_prf_funcs[i].task_id != TASK_NONE )
    {
        if( cust_prf_funcs[i].task_id == dest_id)
        {
            if ( cust_prf_funcs[i].att_db != NULL)
            {
                att_db = cust_prf_funcs[i].att_db;
                break;
            } else i++;
        } else i++;
    }

    if ( att_db != NULL )
    {        
        // Create a Database
        status = attm_svc_create_db_128(&(custs2_env.shdl), param->cfg_flag, param->max_nb_att,
                                        param->att_tbl, dest_id, att_db);
        
        if (status == ATT_ERR_NO_ERROR)
        {
            // save max number of attibutes
            custs2_env.max_nb_att = param->max_nb_att;
            
            // Disable wpt service
            attmdb_svc_set_permission(custs2_env.shdl, PERM(SVC, DISABLE));

            // If we are here, database has been fulfilled with success, go to idle test
            ke_state_set(TASK_CUSTS2, CUSTS2_IDLE);
        }
        
        // Send response to application
        struct custs2_create_db_cfm* cfm = KE_MSG_ALLOC(CUSTS2_CREATE_DB_CFM, src_id, TASK_CUSTS2,
                                                          custs2_create_db_cfm);
        cfm->status = status;
        ke_msg_send(cfm);
    }
    
    return (KE_MSG_CONSUMED);
}

/**
 ****************************************************************************************
 * @brief Handles reception of the @ref CUSTS2_ENABLE_REQ message.
 * @param[in] msgid Id of the message received (probably unused).
 * @param[in] param Pointer to the parameters of the message.
 * @param[in] dest_id ID of the receiving task instance (probably unused).
 * @param[in] src_id ID of the sending task instance.
 * @return If the message was consumed or not.
 ****************************************************************************************
 */
static int custs2_enable_req_handler(ke_msg_id_t const msgid,
                                   struct custs2_enable_req const *param,
                                   ke_task_id_t const dest_id,
                                   ke_task_id_t const src_id)
{
    //Save the connection handle associated to the profile
    custs2_env.con_info.conidx = gapc_get_conidx(param->conhdl);
    //Save the application id
    custs2_env.con_info.appid = src_id;

    // Check if the provided connection exist
    if (custs2_env.con_info.conidx == GAP_INVALID_CONIDX)
    {
        // The connection doesn't exist, request disallowed
        prf_server_error_ind_send((prf_env_struct *)&custs2_env, PRF_ERR_REQ_DISALLOWED,
                                  CUSTS2_ERROR_IND, CUSTS2_ENABLE_REQ);
    }
    else
    {
        //Enable Attributes + Set Security Level
        attmdb_svc_set_permission(custs2_env.shdl, param->sec_lvl);
      
        // Go to connected state
        ke_state_set(TASK_CUSTS2, CUSTS2_CONNECTED);
    }
    
    return (KE_MSG_CONSUMED);
}

/**
 ****************************************************************************************
 * @brief Handles reception of the @ref GATT_WRITE_CMD_IND message.
 * @param[in] msgid Id of the message received (probably unused).
 * @param[in] param Pointer to the parameters of the message.
 * @param[in] dest_id ID of the receiving task instance (probably unused).
 * @param[in] src_id ID of the sending task instance.
 * @return If the message was consumed or not.
 ****************************************************************************************
 */

static int gattc_write_cmd_ind_handler(ke_msg_id_t const msgid,
                                      struct gattc_write_cmd_ind const *param,
                                      ke_task_id_t const dest_id,
                                      ke_task_id_t const src_id)
{
    uint16_t att_idx, value_hdl;
    uint8_t status = PRF_ERR_OK;
    uint8_t uuid[GATT_UUID_128_LEN];
    uint8_t uuid_len;
    att_size_t len;
    uint8_t *value;
    uint16_t perm;
    
    
    if (KE_IDX_GET(src_id) == custs2_env.con_info.conidx)
    {
        att_idx = param->handle - custs2_env.shdl;
        
        if( att_idx < custs2_env.max_nb_att )
        {   
            // Retrieve UUID
            attmdb_att_get_uuid(param->handle, &uuid_len, &(uuid[0]));
            
            // in case of Client Characteristic Configuration, check validity and set value
            if ((uint16_t)*(uint16_t *)&uuid[0] == ATT_DESC_CLIENT_CHAR_CFG)            
            {                
                // Find the handle of the Characteristic Value
                value_hdl = get_value_handle( param->handle );
                if ( !value_hdl ) ASSERT_ERR(0);
                
                // Get permissions to identify if it is NTF or IND.
                attmdb_att_get_permission(value_hdl, &perm);
                status = check_client_char_cfg(PERM_IS_SET(perm, NTF, ENABLE), param);
                
                if (status == PRF_ERR_OK)
                {
                    // Set Client Characteristic Configuration value
                    status = attmdb_att_set_value(param->handle, param->length, (uint8_t*)&(param->value[0]));
                }
            }
            else
            {
                // Call the application function to validate the value before it is written to database
                uint8_t i = 0;
                
                status = PRF_ERR_OK;
                while( cust_prf_funcs[i].task_id != TASK_NONE )
                {
                    if( cust_prf_funcs[i].task_id == dest_id)
                    {
                        if ( cust_prf_funcs[i].value_wr_validation_func != NULL)
                        {
                            status = cust_prf_funcs[i].value_wr_validation_func(att_idx, param->last, param->offset, param->length, (uint8_t *)&param->value[0]);
                            break;
                        } else i++;
                    } else i++;
                }
            
                if (status == PRF_ERR_OK)
                {                    
                    if (param->offset == 0)
                    {
                        // Set value in the database
                        status = attmdb_att_set_value(param->handle, param->length, (uint8_t *)&param->value[0]);
                    }
                    else
                    {
                        // Update value in the database            
                        status = attmdb_att_update_value(param->handle, param->length, param->offset,
                                                            (uint8_t *)&param->value[0]);
                    }
                }
            }
            
            if( (param->last) && (status == PRF_ERR_OK) )
            {
                // Get the value size and data. Can not use param->value, it might be a long value
                if( attmdb_att_get_value(param->handle, &len, &value) != ATT_ERR_NO_ERROR )
                {
                    ASSERT_ERR(0);
                }
                
                //Inform APP                            
                struct custs2_val_write_ind *req_id = KE_MSG_ALLOC_DYN(CUSTS2_VAL_WRITE_IND,
                                                        custs2_env.con_info.appid, custs2_env.con_info.prf_id,
                                                        custs2_val_write_ind,
                                                        len);
                memcpy(req_id->value, (uint8_t*)&value[0], len);
                req_id->conhdl = gapc_get_conhdl(custs2_env.con_info.conidx);    
                req_id->handle = att_idx;
                req_id->length = len;

                ke_msg_send(req_id);
            }                
        }
        else
        {
            status = PRF_ERR_INEXISTENT_HDL;
        }
        
        // Send Write Response only if client requests for RSP (ignored when 'Write Without Response' is used)
        if (param->response == 1)
        {
            // Send Write Response
            atts_write_rsp_send(custs2_env.con_info.conidx, param->handle, status);
        }
    }
       
    return (KE_MSG_CONSUMED);
    
}    
        
/**
 ****************************************************************************************
 * @brief Handles @ref GATTC_CMP_EVT for GATTC_NOTIFY/GATTC_INDICATE messages meaning that
 * notification/indications has been correctly sent to peer device (but not confirmed by peer device).
 * *
 * @param[in] msgid     Id of the message received.
 * @param[in] param     Pointer to the parameters of the message.
 * @param[in] dest_id   ID of the receiving task instance
 * @param[in] src_id    ID of the sending task instance.
 * @return If the message was consumed or not.
 ****************************************************************************************
 */
static int gattc_cmp_evt_handler(ke_msg_id_t const msgid,  struct gattc_cmp_evt const *param,
                                 ke_task_id_t const dest_id, ke_task_id_t const src_id)
{

    if (param->req_type == GATTC_NOTIFY)
    {
        // Send CFM to APP that value has been sent or not
        struct custs2_val_ntf_cfm *cfm = KE_MSG_ALLOC(  CUSTS2_VAL_NTF_CFM, 
                                                            custs2_env.con_info.appid, custs2_env.con_info.prf_id,
                                                            custs2_val_ntf_cfm);

        cfm->handle = custs2_env.ntf_handle;
        cfm->conhdl = gapc_get_conhdl(custs2_env.con_info.conidx);
        cfm->status = param->status;
        
        ke_msg_send(cfm);
    }
    else if (param->req_type == GATTC_INDICATE)
    {
        // Send CFM to APP that value has been sent or not
        struct custs2_val_ind_cfm *cfm = KE_MSG_ALLOC(  CUSTS2_VAL_IND_CFM, 
                                                            custs2_env.con_info.appid, custs2_env.con_info.prf_id,
                                                            custs2_val_ind_cfm);
        cfm->handle = custs2_env.ind_handle;
        cfm->conhdl = gapc_get_conhdl(custs2_env.con_info.conidx);
        cfm->status = param->status;
        
        ke_msg_send(cfm);
    }
    
    return (KE_MSG_CONSUMED);
}

/**
 ****************************************************************************************
 * @brief Handles reception of the @ref CUSTS2_VAL_SET_REQ message. 
 * @param[in] msgid Id of the message received (probably unused).
 * @param[in] param Pointer to the parameters of the message.
 * @param[in] dest_id ID of the receiving task instance (probably unused).
 * @param[in] src_id ID of the sending task instance.
 * @return If the message was consumed or not.
 ****************************************************************************************
 */
static int custs2_val_set_req_handler(ke_msg_id_t const msgid,
                                             struct custs2_val_set_req const *param,
                                             ke_task_id_t const dest_id,
                                             ke_task_id_t const src_id)
{
    
    if (param->conhdl == gapc_get_conhdl(custs2_env.con_info.conidx))
    {
        // Update value in DB
        attmdb_att_set_value(custs2_env.shdl + param->handle, param->length, (uint8_t *)&param->value);
    }
    else
    {
        //PRF_ERR_INVALID_PARAM;
    }
    
    return (KE_MSG_CONSUMED);
}

/**
 ****************************************************************************************
 * @brief Handles reception of the @ref CUSTS2_VAL_NTF_REQ message. 
 * @param[in] msgid Id of the message received (probably unused).
 * @param[in] param Pointer to the parameters of the message.
 * @param[in] dest_id ID of the receiving task instance (probably unused).
 * @param[in] src_id ID of the sending task instance.
 * @return If the message was consumed or not.
 ****************************************************************************************
 */
static int custs2_val_ntf_req_handler(ke_msg_id_t const msgid,
                                             struct custs2_val_ntf_req const *param,
                                             ke_task_id_t const dest_id,
                                             ke_task_id_t const src_id)
{ 
    uint16_t cfg_hdl;
    uint8_t status = PRF_ERR_OK;
    
    if (param->conhdl == gapc_get_conhdl(custs2_env.con_info.conidx))
    {
        uint8_t *cfg_val;
        att_size_t length;

        // Update value in DB
        attmdb_att_set_value(custs2_env.shdl + param->handle, param->length, (uint8_t *)&param->value);

        // Find the handle of the Characteristic Client Configuration
        cfg_hdl = get_cfg_handle( custs2_env.shdl + param->handle, custs2_env.max_nb_att );
        if ( !cfg_hdl ) ASSERT_ERR(0);
        
        // Check if notifications are enabled. 
        attmdb_att_get_value(cfg_hdl, &length, &cfg_val);
        
        // Send indication through GATT
        if ((uint16_t)*((uint16_t*)&cfg_val[0]) == PRF_CLI_START_NTF)
        {
            prf_server_send_event((prf_env_struct *)&custs2_env, 0, custs2_env.shdl + param->handle);
            custs2_env.ntf_handle = param->handle;
        }
        else
        {
            status = PRF_ERR_IND_DISABLED;
        }
    }
    else
    {
        status = PRF_ERR_INVALID_PARAM;
    }
    
    if (status != PRF_ERR_OK)
    {

        // Send CFM to APP that value has been sent or not
        struct custs2_val_ntf_cfm *cfm = KE_MSG_ALLOC( CUSTS2_VAL_NTF_CFM,
                                                   custs2_env.con_info.appid, custs2_env.con_info.prf_id,
                                                   custs2_val_ntf_cfm);
    
        cfm->handle = param->handle;
        cfm->conhdl = gapc_get_conhdl(custs2_env.con_info.conidx);
        cfm->status = status;

        ke_msg_send(cfm);               
    }
    
    return (KE_MSG_CONSUMED);
}

/**
 ****************************************************************************************
 * @brief Handles reception of the @ref CUSTS2_VAL_IND_REQ message. 
 * @param[in] msgid Id of the message received (probably unused).
 * @param[in] param Pointer to the parameters of the message.
 * @param[in] dest_id ID of the receiving task instance (probably unused).
 * @param[in] src_id ID of the sending task instance.
 * @return If the message was consumed or not.
 ****************************************************************************************
 */
static int custs2_val_ind_req_handler(ke_msg_id_t const msgid,
                                             struct custs2_val_ind_req const *param,
                                             ke_task_id_t const dest_id,
                                             ke_task_id_t const src_id)
{
    uint16_t cfg_hdl;
    uint8_t status = PRF_ERR_OK;
    
    if (param->conhdl == gapc_get_conhdl(custs2_env.con_info.conidx))
    {
        uint8_t *cfg_val;
        att_size_t length;

        // Update value in DB
        attmdb_att_set_value(custs2_env.shdl + param->handle, param->length, (uint8_t *)&param->value);

        // Find the handle of the Characteristic Client Configuration
        cfg_hdl = get_cfg_handle( custs2_env.shdl + param->handle, custs2_env.max_nb_att);
        if ( !cfg_hdl ) ASSERT_ERR(0);
        
        // Check if indications are enabled.
        attmdb_att_get_value(cfg_hdl, &length, &cfg_val);
        
        // Send indication through GATT
        if ((uint16_t)*((uint16_t*)&cfg_val[0]) == PRF_CLI_START_IND)
        {
            prf_server_send_event((prf_env_struct *)&custs2_env, 1, custs2_env.shdl + param->handle);
            custs2_env.ind_handle = param->handle;
        }
        else
        {
            status = PRF_ERR_IND_DISABLED;
        }
    }
    else
    {
        status = PRF_ERR_INVALID_PARAM;
    }
    
    if (status != PRF_ERR_OK)
    {

        // Send CFM to APP that value has been sent or not
        struct custs2_val_ind_cfm *cfm = KE_MSG_ALLOC( CUSTS2_VAL_IND_CFM,
                                                   custs2_env.con_info.appid, custs2_env.con_info.prf_id,
                                                   custs2_val_ind_cfm);
    
        cfm->handle = param->handle;
        cfm->conhdl = gapc_get_conhdl(custs2_env.con_info.conidx);
        cfm->status = status;

        ke_msg_send(cfm);               
    }
    
    return (KE_MSG_CONSUMED);
}

static int gapc_disconnect_ind_handler(ke_msg_id_t const msgid,
                                      struct gapc_disconnect_ind const *param,
                                      ke_task_id_t const dest_id,
                                      ke_task_id_t const src_id)
{
    // Check Connection Handle
    if (KE_IDX_GET(src_id) == custs2_env.con_info.conidx)
    {
        custs2_disable(param->conhdl);
    }
    
    return (KE_MSG_CONSUMED);
}

/*
 * GLOBAL VARIABLE DEFINITIONS
 ****************************************************************************************
 */

///Disabled State handler definition.
const struct ke_msg_handler custs2_disabled[] =
{
    {CUSTS2_CREATE_DB_REQ,         (ke_msg_func_t)custs2_create_db_req_handler},
};

///Idle State handler definition.
const struct ke_msg_handler custs2_idle[] =
{
    {CUSTS2_ENABLE_REQ,            (ke_msg_func_t)custs2_enable_req_handler},
};

/// Default State handlers definition
const struct ke_msg_handler custs2_connected[] =
{
    {GATTC_WRITE_CMD_IND,           (ke_msg_func_t)gattc_write_cmd_ind_handler},
    {GATTC_CMP_EVT,                 (ke_msg_func_t)gattc_cmp_evt_handler},
    {CUSTS2_VAL_NTF_REQ,            (ke_msg_func_t)custs2_val_ntf_req_handler},
    {CUSTS2_VAL_SET_REQ,            (ke_msg_func_t)custs2_val_set_req_handler},
    {CUSTS2_VAL_IND_REQ,            (ke_msg_func_t)custs2_val_ind_req_handler},
};

/// Default State handlers definition
const struct ke_msg_handler custs2_default_state[] =
{
    {GAPC_DISCONNECT_IND,           (ke_msg_func_t)gapc_disconnect_ind_handler},
};

/// Specifies the message handler structure for every input state.
const struct ke_state_handler custs2_state_handler[CUSTS2_STATE_MAX] =
{
    [CUSTS2_DISABLED]    = KE_STATE_HANDLER(custs2_disabled),
    [CUSTS2_IDLE]        = KE_STATE_HANDLER(custs2_idle),
    [CUSTS2_CONNECTED]   = KE_STATE_HANDLER(custs2_connected),
};

///Specifies the message handlers that are common to all states.
const struct ke_state_handler custs2_default_handler = KE_STATE_HANDLER(custs2_default_state);

///Defines the place holder for the states of all the task instances.
ke_state_t custs2_state[CUSTS2_IDX_MAX] __attribute__((section("retention_mem_area0"),zero_init)); //@RETENTION MEMORY

#endif // BLE_CUSTOM2_SERVER
