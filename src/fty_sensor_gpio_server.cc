/*  =========================================================================
    fty_sensor_gpio_server - Actor

    Copyright (C) 2014 - 2017 Eaton                                        
                                                                           
    This program is free software; you can redistribute it and/or modify   
    it under the terms of the GNU General Public License as published by   
    the Free Software Foundation; either version 2 of the License, or      
    (at your option) any later version.                                    
                                                                           
    This program is distributed in the hope that it will be useful,        
    but WITHOUT ANY WARRANTY; without even the implied warranty of         
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          
    GNU General Public License for more details.                           
                                                                           
    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.            
    =========================================================================
*/

/*
@header
    fty_sensor_gpio_server - Actor
@discuss
@end
*/

#include "fty_sensor_gpio_classes.h"

//  Structure of our class

struct _fty_sensor_gpio_server_t {
    bool               verbose;       // is actor verbose or not
    char               *name;         // actor name
    mlm_client_t       *mlm;          // malamute client
    libgpio_t          *gpio_lib;     // GPIO library handle
};

// Configuration accessors
// FIXME: why do we need that? zconfig_get should already do this, no?
const char*
s_get (zconfig_t *config, const char* key, std::string &dfl) {
    assert (config);
    char *ret = zconfig_get (config, key, dfl.c_str());
    if (!ret || streq (ret, ""))
        return (char*)dfl.c_str();
    return ret;
}

const char*
s_get (zconfig_t *config, const char* key, const char*dfl) {
    assert (config);
    char *ret = zconfig_get (config, key, dfl);
    if (!ret || streq (ret, ""))
        return dfl;
    return ret;
}

//  --------------------------------------------------------------------------
//  Publish status of the pointed GPIO sensor

void publish_status (fty_sensor_gpio_server_t *self, _gpx_info_t *sensor, int ttl)
{
    zsys_debug ("Publishing GPIO sensor %i (%s) status",
        sensor->gpx_number, sensor->asset_name);

/*
--------------------------------------------------------------------------------
stream=_METRICS_SENSOR
sender=agent-sensor-gpio@rackcontroller-3
subject=status.GPIx@rackcontroller-3
D: 17-03-23 13:14:11 FTY_PROTO_METRIC:
D: 17-03-23 13:14:11     aux=
D: 17-03-23 13:14:11         port=GPIx
D: 17-03-23 13:14:11     time=1490274851
D: 17-03-23 13:14:11     ttl=300
D: 17-03-23 13:14:11     type='status.GPIx'
D: 17-03-23 13:14:11     name='rackcontroller-3' => or name (assetname)?
D: 17-03-23 13:14:11     value='closed'
D: 17-03-23 13:14:11     unit=''
--------------------------------------------------------------------------------

std::string Sensor::topicSuffix () const
{
        return "." + port() + "@" + _location;
*/
        zhash_t *aux = zhash_new ();
        zhash_autofree (aux);
        char* port = (char*)malloc(5);
        sprintf(port, "GPI%i", sensor->gpx_number);
        zhash_insert (aux, "port", (void*) port);
        string msg_type = string("status.") + port;

        zmsg_t *msg = fty_proto_encode_metric (
            aux,
            time (NULL),
            ttl,
            msg_type.c_str (),
            sensor->asset_name, //sensor->location,
            libgpio_get_status_string(sensor->current_state).c_str(),
            "");
        zhash_destroy (&aux);
        if (msg) {
            std::string topic = string("status.") + port + string("@") + sensor->asset_name; //sensor->location;

            zsys_debug ("\tPort: %s, type: %s, status: %s",
                port, msg_type.c_str(),
                libgpio_get_status_string(sensor->current_state).c_str());

            int r = mlm_client_send (self->mlm, topic.c_str (), &msg);
            if( r != 0 )
                zsys_debug("failed to send measurement %s result %", topic.c_str(), r);
            zmsg_destroy (&msg);
        }
        free(port);
}

//  --------------------------------------------------------------------------
//  Check GPIO status and generate alarms if needed

static void
s_check_gpio_status(fty_sensor_gpio_server_t *self)
{
    zsys_debug ("%s", __func__);

    // number of sensors monitored in gpx_list
    zlistx_t *gpx_list = get_gpx_list();
    if (!gpx_list) {
        zsys_debug ("GPx list not initialized, skipping");
        return;
    }
    int sensors_count = zlistx_size (gpx_list);
    _gpx_info_t *gpx_info = NULL;

    if (sensors_count == 0) {
        zsys_debug ("No sensors monitored");
        return;
    }
    else
        zsys_debug ("%i sensor(s) monitored", sensors_count);

    if(!mlm_client_connected(self->mlm))
        return;

    // Acquire the current sensor
    gpx_info = (_gpx_info_t *)zlistx_first (gpx_list);

    // Loop on all sensors
    for (int cur_sensor_num = 0; cur_sensor_num < sensors_count; cur_sensor_num++) {

        // Get the current sensor status
        gpx_info->current_state = libgpio_read(self->gpio_lib, gpx_info->gpx_number);
        if (gpx_info->current_state == GPIO_STATE_UNKNOWN) {
            zsys_debug ("Can't read GPI sensor #%i status", gpx_info->gpx_number);
            continue;
        }
        zsys_debug ("Read '%s' (value: %i) on GPx sensor #%i (%s/%s)",
            libgpio_get_status_string(gpx_info->current_state).c_str(),
            gpx_info->current_state, gpx_info->gpx_number,
            gpx_info->ext_name, gpx_info->asset_name);

        publish_status (self, gpx_info, 300);

        gpx_info = (_gpx_info_t *)zlistx_next (gpx_list);
    }
}

//  --------------------------------------------------------------------------
//  process message from MAILBOX DELIVER INFO INFO/INFO-TEST
//  body :
//    - name    IPC (12378)
//    - type    _https._tcp.
//    - subtype _powerservice._sub._https._tcp.
//    - port    443
//    - hashtable : TXT name, TXT value
//          uuid
//          name
//          vendor
//          serial
//          model
//          location
//          version
//          path
//          protocol format
//          type
//          version
void static
s_handle_mailbox(fty_sensor_gpio_server_t* self, zmsg_t *message)
{
    char *command = zmsg_popstr (message);
    if (!command) {
        zmsg_destroy (&message);
        zsys_warning ("Empty subject.");
        return;
    }
    //we assume all request command are MAILBOX DELIVER, and subject="gpio"
    if (!streq(command, "GPIO") && !streq(command, "GPIO-TEST")) {
        zsys_warning ("%s: Received unexpected command '%s'", self->name, command);
        zmsg_t *reply = zmsg_new ();
        zmsg_addstr(reply, "ERROR");
        zmsg_addstr (reply, "unexpected command");
        mlm_client_sendto (self->mlm, mlm_client_sender (self->mlm), "info", NULL, 1000, &reply);
        zstr_free (&command);
        zmsg_destroy (&message);
        return;
    }
    else {
        zsys_debug ("%s: do '%s'", self->name, command);
//        zmsg_t *reply = zmsg_new ();
//        char *zuuid = zmsg_popstr (message);
//        fty_info_t *info;
        if (streq(command, "GPIO")) {
            ; // info = fty_info_new (self->resolver);
        }
        if (streq(command, "GPIO-TEST")) {
            ; // info = fty_info_test_new ();
        }
        //prepare replied message content
/*        zmsg_addstrf (reply, "%s", zuuid);
        char *srv_name = s_get_name(SRV_NAME, info->uuid);
        zmsg_addstr (reply, srv_name);
        zmsg_addstr (reply, SRV_TYPE);
        zmsg_addstr (reply, SRV_STYPE);
        zmsg_addstr (reply, SRV_PORT);
        zhash_insert(info->infos, INFO_UUID, info->uuid);
        zhash_insert(info->infos, INFO_HOSTNAME, info->hostname);
        zhash_insert(info->infos, INFO_NAME, info->name);
        zhash_insert(info->infos, INFO_NAME_URI, info->name_uri);
        zhash_insert(info->infos, INFO_VENDOR, info->vendor);
        zhash_insert(info->infos, INFO_MODEL, info->model);
        zhash_insert(info->infos, INFO_SERIAL, info->serial);
        zhash_insert(info->infos, INFO_LOCATION, info->location);
        zhash_insert(info->infos, INFO_PARENT_URI, info->parent_uri);
        zhash_insert(info->infos, INFO_VERSION, info->version);
        zhash_insert(info->infos, INFO_REST_PATH, info->path);
        zhash_insert(info->infos, INFO_PROTOCOL_FORMAT, info->protocol_format);
        zhash_insert(info->infos, INFO_TYPE, info->type);
        zhash_insert(info->infos, INFO_TXTVERS, info->txtvers);

        zframe_t * frame_infos = zhash_pack(info->infos);
        zmsg_append (reply, &frame_infos);
        mlm_client_sendto (self->client, mlm_client_sender (self->client), "info", NULL, 1000, &reply);
        zframe_destroy(&frame_infos);
        zstr_free (&zuuid);
        zstr_free(&srv_name);
        fty_info_destroy (&info);
*/
    }
    zstr_free (&command);
    zmsg_destroy (&message);

}

//  --------------------------------------------------------------------------
//  Create a new fty_sensor_gpio_server

fty_sensor_gpio_server_t *
fty_sensor_gpio_server_new (const char* name)
{
    fty_sensor_gpio_server_t *self = (fty_sensor_gpio_server_t *) zmalloc (sizeof (fty_sensor_gpio_server_t));
    assert (self);

    //  Initialize class properties
    self->mlm         = mlm_client_new();
    self->name        = strdup(name);
    self->verbose     = false;

    self->gpio_lib = libgpio_new ();
    assert (self->gpio_lib);

    return self;
}


//  --------------------------------------------------------------------------
//  Destroy the fty_sensor_gpio_server

void
fty_sensor_gpio_server_destroy (fty_sensor_gpio_server_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        fty_sensor_gpio_server_t *self = *self_p;

        //  Free class properties
        libgpio_destroy (&self->gpio_lib);
        free(self->name);
        mlm_client_destroy (&self->mlm);

        //  Free object itself
        free (self);
        *self_p = NULL;
    }
}

//  --------------------------------------------------------------------------
//  Create a new fty_info_server

void
fty_sensor_gpio_server (zsock_t *pipe, void *args)
{
    char *name = (char *)args;
    if (!name) {
        zsys_error ("Adress for fty-sensor-gpio actor is NULL");
        return;
    }

    fty_sensor_gpio_server_t *self = fty_sensor_gpio_server_new(name);
    assert (self);

    zpoller_t *poller = zpoller_new (pipe, mlm_client_msgpipe (self->mlm), NULL);
    assert (poller);

    zsock_signal (pipe, 0); 
    zsys_info ("%s_server: Started", self->name);

    while (!zsys_interrupted)
    {
        void *which = zpoller_wait (poller, TIMEOUT_MS);
        if (which == NULL) {
            if (zpoller_terminated (poller) || zsys_interrupted) {
                break;
            }
        }
        if (which == pipe) {
            zmsg_t *message = zmsg_recv (pipe);
            char *cmd = zmsg_popstr (message);
            zsys_debug ("fty_sensor_gpio: received command %s", cmd);
            if (cmd) {
                if (streq (cmd, "$TERM")) {
                    zstr_free (&cmd);
                    zmsg_destroy (&message);
                    break;
                }
                else if (streq (cmd, "CONNECT")) {
                    char *endpoint = zmsg_popstr (message);
                     if (!endpoint)
                        zsys_error ("%s:\tMissing endpoint", self->name);
                    assert (endpoint);
                    int r = mlm_client_connect (self->mlm, endpoint, 5000, self->name);
                    if (r == -1)
                        zsys_error ("%s:\tConnection to endpoint '%s' failed", self->name, endpoint);
                    zsys_debug("fty-gpio-sensor-server: CONNECT %s/%s", endpoint, self->name);
                    zstr_free (&endpoint);
                }
                else if (streq (cmd, "PRODUCER")) {
                    char *stream = zmsg_popstr (message);
                    assert (stream);
                    mlm_client_set_producer (self->mlm, stream);
                    zsys_debug ("fty_sensor_gpio: setting PRODUCER on %s", stream);
                    zstr_free (&stream);
                }
                else if (streq (cmd, "CONSUMER")) {
                    char *stream = zmsg_popstr (message);
                    char *pattern = zmsg_popstr (message);
                    assert (stream && pattern);
                    mlm_client_set_consumer (self->mlm, stream, pattern);
                    zsys_debug ("fty_sensor_gpio: setting CONSUMER on %s/%s", stream, pattern);
                    zstr_free (&stream);
                    zstr_free (&pattern);
                }
                else if (streq (cmd, "VERBOSE")) {
                    self->verbose = true;
                    zsys_debug ("fty_sensor_gpio: VERBOSE=true");
                }
                else if (streq (cmd, "UPDATE")) {
                    s_check_gpio_status(self);
                }
                else if (streq (cmd, "GPIO_CHIP_ADDRESS")) {
                    char *str_gpio_base_index = zmsg_popstr (message);
                    int gpio_base_index = atoi(str_gpio_base_index);
                    libgpio_set_gpio_base_index (self->gpio_lib, gpio_base_index);
                    zsys_debug ("fty_sensor_gpio: GPIO_CHIP_ADDRESS=%i", gpio_base_index);
                }
                else {
                    zsys_warning ("%s:\tUnknown API command=%s, ignoring", __func__, cmd);
                }
                zstr_free (&cmd);
            }
            zmsg_destroy (&message);
        }
        else if (which == mlm_client_msgpipe (self->mlm)) {
            zmsg_t *message = mlm_client_recv (self->mlm);
            if (streq (mlm_client_command (self->mlm), "MAILBOX DELIVER")) {
                // FIXME: to be removed?
                //s_handle_stream (self, message);
            } else if (streq (mlm_client_command (self->mlm), "MAILBOX DELIVER")) {
                // someone is addressing us directly
                // FIXME: can be requested for:
                // * sensors manifest (pn, type, normal status) by UI
                // * GPO interacting
                s_handle_mailbox(self, message);
            }
            zmsg_destroy (&message);
        }
    }
    zpoller_destroy (&poller);
    mlm_client_destroy (&self->mlm);
}

//  --------------------------------------------------------------------------
//  Self test of this class

// FIXME

void
fty_sensor_gpio_server_test (bool verbose)
{
    printf (" * fty_sensor_gpio_server: ");

    //  @selftest
    //  Simple create/destroy test

    // Note: If your selftest reads SCMed fixture data, please keep it in
    // src/selftest-ro; if your test creates filesystem objects, please
    // do so under src/selftest-rw. They are defined below along with a
    // usecase for the variables (assert) to make compilers happy.
    const char *SELFTEST_DIR_RO = "src/selftest-ro";
    const char *SELFTEST_DIR_RW = "src/selftest-rw";
    assert (SELFTEST_DIR_RO);
    assert (SELFTEST_DIR_RW);
    // Uncomment these to use C++ strings in C++ selftest code:
    //std::string str_SELFTEST_DIR_RO = std::string(SELFTEST_DIR_RO);
    //std::string str_SELFTEST_DIR_RW = std::string(SELFTEST_DIR_RW);
    //assert ( (str_SELFTEST_DIR_RO != "") );
    //assert ( (str_SELFTEST_DIR_RW != "") );
    // NOTE that for "char*" context you need (str_SELFTEST_DIR_RO + "/myfilename").c_str()

    fty_sensor_gpio_server_t *self = fty_sensor_gpio_server_new (FTY_SENSOR_GPIO_AGENT);
    assert (self);
    fty_sensor_gpio_server_destroy (&self);
    //  @end
    printf ("OK\n");
}

#if 0
void
fty_info_server_test (bool verbose)
{
    printf (" * fty_info_server_test: ");

    //  @selftest

    static const char* endpoint = "inproc://fty-info-test";

    zactor_t *server = zactor_new (mlm_server, (void*) "Malamute");
    zstr_sendx (server, "BIND", endpoint, NULL);
    if (verbose)
        zstr_send (server, "VERBOSE");

    mlm_client_t *client = mlm_client_new ();
    mlm_client_connect (client, endpoint, 1000, "fty_info_server_test");


    zactor_t *info_server = zactor_new (fty_info_server, (void*) "fty-info");
    if (verbose)
        zstr_send (info_server, "VERBOSE");
    zstr_sendx (info_server, "CONNECT", endpoint, NULL);
    zstr_sendx (info_server, "CONSUMER", FTY_PROTO_STREAM_ASSETS, ".*", NULL);
	zclock_sleep (1000);

    // Test #1: request INFO-TEST
    {
        zsys_debug ("fty-info-test:Test #1");
        zmsg_t *request = zmsg_new ();
        zmsg_addstr (request, "INFO-TEST");
        zuuid_t *zuuid = zuuid_new ();
        zmsg_addstrf (request, "%s", zuuid_str_canonical (zuuid));
        mlm_client_sendto (client, "fty-info", "info", NULL, 1000, &request);

        zmsg_t *recv = mlm_client_recv (client);

        assert (zmsg_size (recv) == 6);
        zsys_debug ("fty-info-test: zmsg_size = %d",zmsg_size (recv));
        char *zuuid_reply = zmsg_popstr (recv);
        assert (zuuid_reply && streq (zuuid_str_canonical(zuuid), zuuid_reply));

        char *srv_name = zmsg_popstr (recv);
        assert (srv_name && streq (srv_name,"IPC (ce7c523e)"));
        zsys_debug ("fty-info-test: srv name = '%s'", srv_name);
        char *srv_type = zmsg_popstr (recv);
        assert (srv_type && streq (srv_type,SRV_TYPE));
        zsys_debug ("fty-info-test: srv type = '%s'", srv_type);
        char *srv_stype = zmsg_popstr (recv);
        assert (srv_stype && streq (srv_stype,SRV_STYPE));
        zsys_debug ("fty-info-test: srv stype = '%s'", srv_stype);
        char *srv_port = zmsg_popstr (recv);
        assert (srv_port && streq (srv_port,SRV_PORT));
        zsys_debug ("fty-info-test: srv port = '%s'", srv_port);

        zframe_t *frame_infos = zmsg_next (recv);
        zhash_t *infos = zhash_unpack(frame_infos);

        char * uuid = (char *) zhash_lookup (infos, INFO_UUID);
        assert(uuid && streq (uuid,TST_UUID));
        zsys_debug ("fty-info-test: uuid = '%s'", uuid);
        char * hostname = (char *) zhash_lookup (infos, INFO_HOSTNAME);
        assert(hostname && streq (hostname, TST_HOSTNAME));
        zsys_debug ("fty-info-test: hostname = '%s'", hostname);
        char * name = (char *) zhash_lookup (infos, INFO_NAME);
        assert(name && streq (name, TST_NAME));
        zsys_debug ("fty-info-test: name = '%s'", name);
        char * name_uri = (char *) zhash_lookup (infos, INFO_NAME_URI);
        assert(name_uri && streq (name_uri, TST_NAME_URI));
        zsys_debug ("fty-info-test: name_uri = '%s'", name_uri);
        char * vendor = (char *) zhash_lookup (infos, INFO_VENDOR);
        assert(vendor && streq (vendor, TST_VENDOR));
        zsys_debug ("fty-info-test: vendor = '%s'", vendor);
        char * serial = (char *) zhash_lookup (infos, INFO_SERIAL);
        assert(serial && streq (serial, TST_SERIAL));
        zsys_debug ("fty-info-test: serial = '%s'", serial);
        char * model = (char *) zhash_lookup (infos, INFO_MODEL);
        assert(model && streq (model, TST_MODEL));
        zsys_debug ("fty-info-test: model = '%s'", model);
        char * location = (char *) zhash_lookup (infos, INFO_LOCATION);
        assert(location && streq (location, TST_LOCATION));
        zsys_debug ("fty-info-test: location = '%s'", location);
        char * location_uri = (char *) zhash_lookup (infos, INFO_PARENT_URI);
        assert(location_uri && streq (location_uri, TST_PARENT_URI));
        zsys_debug ("fty-info-test: location_uri = '%s'", location_uri);
        char * version = (char *) zhash_lookup (infos, INFO_VERSION);
        assert(version && streq (version, TST_VERSION));
        zsys_debug ("fty-info-test: version = '%s'", version);
        char * rest_root = (char *) zhash_lookup (infos, INFO_REST_PATH);
        assert(rest_root && streq (rest_root, TXT_PATH));
        zsys_debug ("fty-info-test: rest_path = '%s'", rest_root);
        zstr_free (&zuuid_reply);
        zstr_free (&srv_name);
        zstr_free (&srv_type);
        zstr_free (&srv_stype);
        zstr_free (&srv_port);
        zhash_destroy(&infos);
        zmsg_destroy (&recv);
        zmsg_destroy (&request);
        zuuid_destroy (&zuuid);
        zsys_info ("fty-info-test:Test #1: OK");
    }
    // Test #2: request INFO
    {
        zsys_debug ("fty-info-test:Test #2");
        zmsg_t *request = zmsg_new ();
        zmsg_addstr (request, "INFO");
        zuuid_t *zuuid = zuuid_new ();
        zmsg_addstrf (request, "%s", zuuid_str_canonical (zuuid));
        mlm_client_sendto (client, "fty-info", "INFO", NULL, 1000, &request);

        zmsg_t *recv = mlm_client_recv (client);

        assert (zmsg_size (recv) == 6);
        char *zuuid_reply = zmsg_popstr (recv);
        assert (zuuid_reply && streq (zuuid_str_canonical(zuuid), zuuid_reply));

        char *srv_name  = zmsg_popstr (recv);
        char *srv_type  = zmsg_popstr (recv);
        char *srv_stype = zmsg_popstr (recv);
        char *srv_port  = zmsg_popstr (recv);

        zframe_t *frame_infos = zmsg_next (recv);
        zhash_t *infos = zhash_unpack(frame_infos);

        char *value = (char *) zhash_first (infos);   // first value
        while ( value != NULL )  {
            char *key = (char *) zhash_cursor (infos);   // key of this value
            zsys_debug ("fty-info-test: %s = %s",key,value);
            value     = (char *) zhash_next (infos);   // next value
        }
        zstr_free (&zuuid_reply);
        zstr_free (&srv_name);
        zstr_free (&srv_type);
        zstr_free (&srv_stype);
        zstr_free (&srv_port);
        zhash_destroy(&infos);
        zmsg_destroy (&recv);
        zmsg_destroy (&request);
        zuuid_destroy (&zuuid);
        zsys_info ("fty-info-test:Test #2: OK");
    }
    mlm_client_t *asset_generator = mlm_client_new ();
    mlm_client_connect (asset_generator, endpoint, 1000, "fty_info_asset_generator");
    mlm_client_set_producer (asset_generator, FTY_PROTO_STREAM_ASSETS);
    // Test #3: process asset message - CREATE RC
    {
        zsys_debug ("fty-info-test:Test #3");
        const char *name = TST_NAME;
        const char *parent = TST_PARENT_INAME;
        zhash_t* aux = zhash_new ();
        zhash_t *ext = zhash_new ();
        zhash_autofree (aux);
        zhash_autofree (ext);
        zhash_update (aux, "type", (void *) "device");
	    zhash_update (aux, "subtype", (void *) "rackcontroller");
	    zhash_update (aux, "parent", (void *) parent);
        zhash_update (ext, "name", (void *) name);
        zhash_update (ext, "ip.1", (void *) "127.0.0.1");

        zmsg_t *msg = fty_proto_encode_asset (
                aux,
                TST_INAME,
                FTY_PROTO_ASSET_OP_CREATE,
                ext);

        int rv = mlm_client_send (asset_generator, "device.rackcontroller@ipc-001", &msg);
        assert (rv == 0);
        zhash_destroy (&aux);
        zhash_destroy (&ext);

        zclock_sleep (1000);

        zmsg_t *request = zmsg_new ();
        zmsg_addstr (request, "INFO");
        zuuid_t *zuuid = zuuid_new ();
        zmsg_addstrf (request, "%s", zuuid_str_canonical (zuuid));
        mlm_client_sendto (client, "fty-info", "INFO", NULL, 1000, &request);

        zmsg_t *recv = mlm_client_recv (client);

        assert (zmsg_size (recv) == 6);
        char *zuuid_reply = zmsg_popstr (recv);
        assert (zuuid_reply && streq (zuuid_str_canonical(zuuid), zuuid_reply));
        char *srv_name  = zmsg_popstr (recv);
        char *srv_type  = zmsg_popstr (recv);
        char *srv_stype = zmsg_popstr (recv);
        char *srv_port  = zmsg_popstr (recv);

        zframe_t *frame_infos = zmsg_next (recv);
        zhash_t *infos = zhash_unpack(frame_infos);

        char *value = (char *) zhash_first (infos);   // first value
        while ( value != NULL )  {
            char *key = (char *) zhash_cursor (infos);   // key of this value
            zsys_debug ("fty-info-test: %s = %s",key,value);
            if (streq (key, INFO_NAME))
                assert (streq (value, TST_NAME));
            if (streq (key, INFO_NAME_URI))
                assert (streq (value, TST_NAME_URI));
            if (streq (key, INFO_PARENT_URI))
                assert (streq (value, TST_PARENT_URI));
            value     = (char *) zhash_next (infos);   // next value
        }
        zstr_free (&zuuid_reply);
        zstr_free (&srv_name);
        zstr_free (&srv_type);
        zstr_free (&srv_stype);
        zstr_free (&srv_port);
        zhash_destroy(&infos);
        zmsg_destroy (&recv);
        zmsg_destroy (&request);
        zuuid_destroy (&zuuid);
        zsys_info ("fty-info-test:Test #3: OK");
    }
    //TEST #4: process asset message - UPDATE RC (change location)
    {
        zsys_debug ("fty-info-test:Test #4");
        zhash_t* aux = zhash_new ();
        zhash_t *ext = zhash_new ();
        zhash_autofree (aux);
        zhash_autofree (ext);
        const char *name = TST_NAME;
        const char *location = TST_PARENT2_INAME;
        zhash_update (aux, "type", (void *) "device");
        zhash_update (aux, "subtype", (void *) "rackcontroller");
        zhash_update (aux, "parent", (void *) location);
        zhash_update (ext, "name", (void *) name);
        zhash_update (ext, "ip.1", (void *) "127.0.0.1");

        zmsg_t *msg = fty_proto_encode_asset (
                aux,
                TST_INAME,
                FTY_PROTO_ASSET_OP_UPDATE,
                ext);

        int rv = mlm_client_send (asset_generator, "device.rackcontroller@ipc-001", &msg);
        assert (rv == 0);
        zhash_destroy (&aux);
        zhash_destroy (&ext);

        zclock_sleep (1000);

        zmsg_t *request = zmsg_new ();
        zmsg_addstr (request, "INFO");
        zuuid_t *zuuid = zuuid_new ();
        zmsg_addstrf (request, "%s", zuuid_str_canonical (zuuid));
        mlm_client_sendto (client, "fty-info", "INFO", NULL, 1000, &request);

        zmsg_t *recv = mlm_client_recv (client);

        assert (zmsg_size (recv) == 6);
        char *zuuid_reply = zmsg_popstr (recv);
        assert (zuuid_reply && streq (zuuid_str_canonical(zuuid), zuuid_reply));
        char *srv_name  = zmsg_popstr (recv);
        char *srv_type  = zmsg_popstr (recv);
        char *srv_stype = zmsg_popstr (recv);
        char *srv_port  = zmsg_popstr (recv);
        zframe_t *frame_infos = zmsg_next (recv);
        zhash_t *infos = zhash_unpack(frame_infos);

        char *value = (char *) zhash_first (infos);   // first value
        while ( value != NULL )  {
            char *key = (char *) zhash_cursor (infos);   // key of this value
            zsys_debug ("fty-info-test: %s = %s",key,value);
            /*if (streq (key, INFO_NAME))
                assert (streq (value, TST_NAME));
            if (streq (key, INFO_NAME_URI))
                assert (streq (value, TST_NAME_URI));
            if (streq (key, INFO_LOCATION_URI))
                assert (streq (value, TST_LOCATION2_URI));*/
            value     = (char *) zhash_next (infos);   // next value
        }
        zstr_free (&zuuid_reply);
        zstr_free (&srv_name);
        zstr_free (&srv_type);
        zstr_free (&srv_stype);
        zstr_free (&srv_port);
        zhash_destroy(&infos);
        zmsg_destroy (&recv);
        zmsg_destroy (&request);
        zuuid_destroy (&zuuid);
        zsys_info ("fty-info-test:Test #4: OK");
    }
    //TEST #5: process asset message - do not process CREATE RC with IP address
    // which does not belong to us
    {
        zsys_debug ("fty-info-test:Test #5");
        zhash_t* aux = zhash_new ();
        zhash_t *ext = zhash_new ();
        zhash_autofree (aux);
        zhash_autofree (ext);
        const char *parent = TST_PARENT_INAME;
        zhash_update (aux, "type", (void *) "device");
        zhash_update (aux, "subtype", (void *) "rack controller");
        zhash_update (aux, "parent", (void *) parent);
        // use invalid IP address to make sure we don't have it
        zhash_update (ext, "ip.1", (void *) "300.3000.300.300");

        zmsg_t *msg = fty_proto_encode_asset (
                aux,
                TST_INAME,
                FTY_PROTO_ASSET_OP_CREATE,
                ext);

        int rv = mlm_client_send (asset_generator, "device.rack controller@ipc-001", &msg);
        assert (rv == 0);
        zhash_destroy (&aux);
        zhash_destroy (&ext);

        zclock_sleep (1000);

        zmsg_t *request = zmsg_new ();
        zmsg_addstr (request, "INFO");
        zuuid_t *zuuid = zuuid_new ();
        zmsg_addstrf (request, "%s", zuuid_str_canonical (zuuid));
        mlm_client_sendto (client, "fty-info", "INFO", NULL, 1000, &request);

        zmsg_t *recv = mlm_client_recv (client);

        assert (zmsg_size (recv) == 6);
        char *zuuid_reply = zmsg_popstr (recv);
        assert (zuuid_reply && streq (zuuid_str_canonical(zuuid), zuuid_reply));
        char *srv_name  = zmsg_popstr (recv);
        char *srv_type  = zmsg_popstr (recv);
        char *srv_stype = zmsg_popstr (recv);
        char *srv_port  = zmsg_popstr (recv);

        zframe_t *frame_infos = zmsg_next (recv);
        zhash_t *infos = zhash_unpack(frame_infos);

        char *value = (char *) zhash_first (infos);   // first value
        while ( value != NULL )  {
            char *key = (char *) zhash_cursor (infos);   // key of this value
            zsys_debug ("fty-info-test: %s = %s",key,value);
            /*if (streq (key, INFO_NAME))
                assert (streq (value, TST_NAME));
            if (streq (key, INFO_NAME_URI))
                assert (streq (value, TST_NAME_URI));
            if (streq (key, INFO_LOCATION_URI))
                assert (streq (value, TST_LOCATION2_URI));*/
            value     = (char *) zhash_next (infos);   // next value
        }
        zstr_free (&zuuid_reply);
        zstr_free (&srv_name);
        zstr_free (&srv_type);
        zstr_free (&srv_stype);
        zstr_free (&srv_port);
        zhash_destroy(&infos);
        zmsg_destroy (&recv);
        zmsg_destroy (&request);
        zuuid_destroy (&zuuid);
        zsys_info ("fty-info-test:Test #5: OK");
    }
    // TEST #6 : test STREAM announce
    {
        zsys_debug ("fty-info-test:Test #6");
        int rv = mlm_client_set_consumer (client, "ANNOUNCE-TEST", ".*");
        assert(rv>=0);
        zstr_sendx (info_server, "PRODUCER", "ANNOUNCE-TEST", NULL);
        zmsg_t *recv = mlm_client_recv (client);
        assert(recv);
        const char *command = mlm_client_command (client);
        assert(streq (command, "STREAM DELIVER"));
        char *srv_name = zmsg_popstr (recv);
        assert (srv_name && streq (srv_name,"IPC (ce7c523e)"));
        zsys_debug ("fty-info-test: srv name = '%s'", srv_name);
        char *srv_type = zmsg_popstr (recv);
        assert (srv_type && streq (srv_type,SRV_TYPE));
        zsys_debug ("fty-info-test: srv type = '%s'", srv_type);
        char *srv_stype = zmsg_popstr (recv);
        assert (srv_stype && streq (srv_stype,SRV_STYPE));
        zsys_debug ("fty-info-test: srv stype = '%s'", srv_stype);
        char *srv_port = zmsg_popstr (recv);
        assert (srv_port && streq (srv_port,SRV_PORT));
        zsys_debug ("fty-info-test: srv port = '%s'", srv_port);

        zframe_t *frame_infos = zmsg_next (recv);
        zhash_t *infos = zhash_unpack(frame_infos);

        char * uuid = (char *) zhash_lookup (infos, INFO_UUID);
        assert(uuid && streq (uuid,TST_UUID));
        zsys_debug ("fty-info-test: uuid = '%s'", uuid);
        char * hostname = (char *) zhash_lookup (infos, INFO_HOSTNAME);
        assert(hostname && streq (hostname, TST_HOSTNAME));
        zsys_debug ("fty-info-test: hostname = '%s'", hostname);
        char * name = (char *) zhash_lookup (infos, INFO_NAME);
        assert(name && streq (name, TST_NAME));
        zsys_debug ("fty-info-test: name = '%s'", name);
        char * name_uri = (char *) zhash_lookup (infos, INFO_NAME_URI);
        assert(name_uri && streq (name_uri, TST_NAME_URI));
        zsys_debug ("fty-info-test: name_uri = '%s'", name_uri);
        char * vendor = (char *) zhash_lookup (infos, INFO_VENDOR);
        assert(vendor && streq (vendor, TST_VENDOR));
        zsys_debug ("fty-info-test: vendor = '%s'", vendor);
        char * serial = (char *) zhash_lookup (infos, INFO_SERIAL);
        assert(serial && streq (serial, TST_SERIAL));
        zsys_debug ("fty-info-test: serial = '%s'", serial);
        char * model = (char *) zhash_lookup (infos, INFO_MODEL);
        assert(model && streq (model, TST_MODEL));
        zsys_debug ("fty-info-test: model = '%s'", model);
        char * location = (char *) zhash_lookup (infos, INFO_LOCATION);
        assert(location && streq (location, TST_LOCATION));
        zsys_debug ("fty-info-test: location = '%s'", location);
        char * location_uri = (char *) zhash_lookup (infos, INFO_PARENT_URI);
        assert(location_uri && streq (location_uri, TST_PARENT_URI));
        zsys_debug ("fty-info-test: location_uri = '%s'", location_uri);
        char * version = (char *) zhash_lookup (infos, INFO_VERSION);
        assert(version && streq (version, TST_VERSION));
        zsys_debug ("fty-info-test: version = '%s'", version);
        char * rest_root = (char *) zhash_lookup (infos, INFO_REST_PATH);
        assert(rest_root && streq (rest_root, TXT_PATH));
        zsys_debug ("fty-info-test: rest_path = '%s'", rest_root);


        zstr_free (&srv_name);
        zstr_free (&srv_type);
        zstr_free (&srv_stype);
        zstr_free (&srv_port);

        zhash_destroy(&infos);
        zmsg_destroy (&recv);
        zsys_info ("fty-info-test:Test #6: OK");

    }
    //TODO: test that we construct topology properly
    //TODO: test that UPDATE message updates the topology properly
    mlm_client_destroy (&asset_generator);
    //  @end
    zactor_destroy (&info_server);
    mlm_client_destroy (&client);
    zactor_destroy (&server);
    zsys_info ("OK\n");
}

#endif // #if 0
