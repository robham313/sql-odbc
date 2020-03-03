/*
 * Copyright <2019> Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 */

/*	TryEnterCriticalSection needs the following #define */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0400
#endif /* _WIN32_WINNT */

#include "es_connection.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "misc.h"

/* for htonl */
#ifdef WIN32
#include <Winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <map>
#include <string>

#include "dlg_specific.h"
#include "environ.h"
#include "es_apifunc.h"
#include "es_helper.h"
#include "loadlib.h"
#include "multibyte.h"
#include "qresult.h"
#include "statement.h"

#define PROTOCOL3_OPTS_MAX 30
#define ERROR_BUFF_SIZE 200
#define OPTION_COUNT 4
#if OPTION_COUNT > PROTOCOL3_OPTS_MAX
#error("Option count (OPTION_COUNT) is greater than max option count allow (PROTOCOL3_OPTS_MAX).")
#endif

void CC_determine_locale_encoding(ConnectionClass *self);

char CC_connect(ConnectionClass *self) {
    if (self == NULL)
        return 0;

    // Attempt to connect to ES
    int conn_code = LIBES_connect(self);
    if (conn_code <= 0)
        return static_cast< char >(conn_code);

    // Set translation and check for errors
    CC_set_translation(self);
    if ((CC_get_errornumber(self) > 0) || (self->status == CONN_DOWN)) {
        const char *err_msg = CC_get_errormsg(self);
        if (err_msg != NULL)
            CC_set_error(self, -1, err_msg, "CC_connect");
        return 0;
    }

    // Set encodings
    CC_determine_locale_encoding(self);
#ifdef UNICODE_SUPPORT
    if (CC_is_in_unicode_driver(self)) {
        if (!SQL_SUCCEEDED(CC_send_client_encoding(self, "UTF8"))) {
            return 0;
        }
    } else
#endif
    {
        if (!SQL_SUCCEEDED(
                CC_send_client_encoding(self, self->locale_encoding))) {
            return 0;
        }
    }

    // Set cursor parameters based on connection info
    ci_updatable_cursors_set(&(self->connInfo));
    self->status = CONN_CONNECTED;
    if ((CC_is_in_unicode_driver(self))
        && ((CC_is_in_ansi_app(self)) || (self->connInfo.bde_environment > 0)))
        self->unicode |= CONN_DISALLOW_WCHAR;

    // 1 is SQL_SUCCESS and 2 is SQL_SCCUESS_WITH_INFO
    return 1;
}

int LIBES_connect(ConnectionClass *self) {
    if (self == NULL)
        return 0;

    // Setup options
    runtime_options rt_opts;

    // Connection
    rt_opts.conn.server.assign(self->connInfo.server);
    rt_opts.conn.port.assign(self->connInfo.port);
    rt_opts.conn.database.assign(self->connInfo.database);
    rt_opts.conn.timeout.assign(self->connInfo.response_timeout);

    // Authentication
    rt_opts.auth.auth_type.assign(self->connInfo.authtype);
    rt_opts.auth.username.assign(self->connInfo.username);
    rt_opts.auth.password.assign(SAFE_NAME(self->connInfo.password));
    rt_opts.auth.region.assign(self->connInfo.region);

    // Encryption
    rt_opts.crypt.verify_server = (self->connInfo.verify_server == 1);
    rt_opts.crypt.use_ssl = (self->connInfo.use_ssl == 1);
    // TODO AE-288: Add settings if required for Mac support

    // Connect to DB with parameters
    try {
        if (self->connInfo.esopt_in_str)
            GetElasticSpecificOpts(rt_opts, SAFE_NAME(self->connInfo.esopt));
    } catch (std::exception &e) {
        CC_set_error(self, CONN_OPENDB_ERROR, e.what(), "LIBES_connect");
        return 0;
    }
    void *esconn = ESConnectDBParams(rt_opts, FALSE, OPTION_COUNT);
    if (esconn == NULL) {
        const char *err = GetErrorMsg(esconn);
        CC_set_error(self, CONN_OPENDB_ERROR,
                     (err == NULL) ? "ESConnectDBParams error" : err,
                     "LIBES_connect");
        return 0;
    }

    // Check connection status
    if (ESStatus(esconn) != CONNECTION_OK) {
        const char *msg = GetErrorMsg(esconn);
        char error_message_out[ERROR_BUFF_SIZE] = "";
        if (msg != NULL)
            SPRINTF_FIXED(
                error_message_out,
                "elasticsearch connection status was not CONNECTION_OK: %s",
                msg);
        else
            STRCPY_FIXED(error_message_out,
                         "elasticsearch connection status was not "
                         "CONNECTION_OK. No error message "
                         "available.");
        CC_set_error(self, CONN_OPENDB_ERROR, error_message_out,
                     "LIBES_connect");
        ESDisconnect(esconn);
        return 0;
    }

    // Set server version
    std::string server_version = GetServerVersion(esconn);
    STRCPY_FIXED(self->es_version, server_version.c_str());

    self->esconn = (void *)esconn;
    return 1;
}

// TODO: When we fix encoding, we should look into returning a code here. This
// is called in connection.c and the return code isn't checked
void CC_set_locale_encoding(ConnectionClass *self, const char *encoding) {
    if (self == NULL)
        return;

    // Set encoding
    char *prev_encoding = self->locale_encoding;
    self->locale_encoding = (encoding == NULL) ? NULL : strdup(encoding);
    if (prev_encoding)
        free(prev_encoding);
}

// TODO: Add return code - see above function comment
void CC_determine_locale_encoding(ConnectionClass *self) {
    // Don't update if it's already set
    if ((self == NULL) || (self->locale_encoding != NULL))
        return;

    // Get current db encoding and derive the locale encoding
    // TODO AE-227: Investigate locale
    CC_set_locale_encoding(self, "SQL_ASCII");
}

int CC_send_client_encoding(ConnectionClass *self, const char *encoding) {
    if ((self == NULL) || (encoding == NULL))
        return SQL_ERROR;

    // Update client encoding
    std::string des_db_encoding(encoding);
    std::string cur_db_encoding = ESGetClientEncoding(self->esconn);
    if (des_db_encoding != cur_db_encoding) {
        if (!ESSetClientEncoding(self->esconn, des_db_encoding)) {
            return SQL_ERROR;
        }
    }

    // Update connection class to reflect updated client encoding
    char *prev_encoding = self->original_client_encoding;
    self->original_client_encoding = strdup(des_db_encoding.c_str());
    self->ccsc = static_cast< short >(es_CS_code(des_db_encoding.c_str()));
    self->mb_maxbyte_per_char = static_cast< short >(es_mb_maxlen(self->ccsc));
    if (prev_encoding != NULL)
        free(prev_encoding);

    return SQL_SUCCESS;
}

// TODO: Use of es_version* is peppered throughout connection.c, convert to
// es_version*
void CC_initialize_es_version(ConnectionClass *self) {
    STRCPY_FIXED(self->es_version, "7.4");
    self->es_version_major = 7;
    self->es_version_minor = 4;
}

void LIBES_disconnect(void *conn) {
    ESDisconnect(conn);
}