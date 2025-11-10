/*
 * Copyright © 2021 Waydroid Project.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Erfan Abdi <erfangplus@gmail.com>
 * Modified by: domin746826 <dominik.sitarski@gmail.com>
 * 
 */

#include "hybrisbindertypes.h"
#include <vector>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <glib.h>
#include <gutil_log.h>
#include <gio/gio.h>
#include <glib-unix.h>


#include <glib.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>

extern "C" {
#include <libssc/libssc-sensor.h>
#include <libssc/libssc-sensor-accelerometer.h>
}

#define RET_OK          (0)
#define RET_NOTFOUND    (1)
#define RET_INVARG      (2)
#define RET_ERR         (3)

#define DEFAULT_DEVICE  "/dev/hwbinder"
#define DEFAULT_IFACE   "android.hardware.sensors@1.0::ISensors"
#define DEFAULT_NAME    "default"

static gboolean
stop_requested(gpointer user_data)
{
    SSCSensorAccelerometer *sensor = static_cast<SSCSensorAccelerometer *>(user_data);
    GError *error = NULL;

    if (!ssc_sensor_accelerometer_close_sync(sensor, NULL, &error)) {
        g_warning("Nie można zatrzymać sensora SSC: %s", error ? error->message : "unknown");
        g_clear_error(&error);
    }

    return G_SOURCE_REMOVE;
}


gfloat accel_x = 0.0f;
gfloat accel_y = 0.0f;
gfloat accel_z = 0.0f;

static void
measurement_cb(SSCSensorAccelerometer *sensor,
               gfloat ax,
               gfloat ay,
               gfloat az,
               gpointer user_data)
{
    (void)sensor;
    (void)user_data;

    // g_print("Accel: %+7.3f %+7.3f %+7.3f m/s²\n", ax, ay, az);
    accel_x = ax;
    accel_y = ay;
    accel_z = az;
    // fflush(stdout);
}

// --- Fake Sensor Data ---
#define FAKE_ACCELEROMETER_HANDLE 1
static bool fake_sensor_activated = true;
static float fake_sensor_phase = 0.0f;
static bool fake_flush_pending = false;
static int fake_flush_handle = FAKE_ACCELEROMETER_HANDLE;
// --- End Fake Sensor Data ---

typedef struct app {
    GMainLoop* loop;
    GBinderServiceManager* sm;
    GBinderLocalObject* obj;
    int ret;
} App;

typedef struct response {
    GBinderRemoteRequest* req;
    GBinderLocalReply* reply;
    int maxCount;
} Response;

static const char logtag[] = "waydroid-sensors-daemon";

static
gboolean
app_signal(
    gpointer user_data)
{
    App* app = (App*) user_data;

    GINFO("Caught signal, shutting down...");
    g_main_loop_quit(app->loop);
    return G_SOURCE_CONTINUE;
}

#define sensors_write_hidl_string_data(writer,ptr,field,index,off) \
     sensors_write_string_with_parent(writer, &ptr->field, index, \
        (off) + ((guint8*)(&ptr->field) - (guint8*)ptr))

static
inline
void
sensors_write_string_with_parent(
    GBinderWriter* writer,
    const GBinderHidlString* str,
    guint32 index,
    guint32 offset)
{
    GBinderParent parent;

    parent.index = index;
    parent.offset = offset;

    /* Strings are NULL-terminated, hence len + 1 */
    gbinder_writer_append_buffer_object_with_parent(writer, str->data.str,
        str->len + 1, &parent);
}

static
void
sensors_write_info_strings(
    GBinderWriter* w,
    const sensor_t* sensor,
    guint idx,
    guint i)
{
    const guint off = sizeof(*sensor) * i;

    /* Write the string data in the right order */
    sensors_write_hidl_string_data(w, sensor, name, idx, off);
    sensors_write_hidl_string_data(w, sensor, vendor, idx, off);
    sensors_write_hidl_string_data(w, sensor, typeAsString, idx, off);
    sensors_write_hidl_string_data(w, sensor, requiredPermission, idx, off);
}

static
gboolean
app_async_resp(
    gpointer user_data)
{
    Response* resp = (Response*)user_data;
    int err = 0;
    GBinderWriter writer;

    guint emitted = 0;
    sensors_event_t* event_data = NULL;

    gbinder_local_reply_init_writer(resp->reply, &writer);

    if (resp->maxCount > 0) {
        sensors_event_t fake_event;
        memset(&fake_event, 0, sizeof(fake_event));

        fake_event.timestamp = g_get_real_time() * 1000; // ns

        if (fake_flush_pending) {
            fake_event.sensorHandle = fake_flush_handle;
            fake_event.sensorType = SENSOR_TYPE_META_DATA;
            fake_event.u.meta.what = META_DATA_FLUSH_COMPLETE;
            fake_flush_pending = false;
        } else if (fake_sensor_activated) {
            // fake_sensor_phase += 0.12f;
            // if (fake_sensor_phase > 6.2831853f)
            //     fake_sensor_phase -= 6.2831853f;

            // const float angle = fake_sensor_phase;

            fake_event.sensorHandle = FAKE_ACCELEROMETER_HANDLE;
            fake_event.sensorType = SENSOR_TYPE_ACCELEROMETER;
            // fake_event.u.vec3.x = 1.5f * static_cast<float>(std::sin(angle));
            // fake_event.u.vec3.y = 1.5f * static_cast<float>(std::cos(angle));
            // fake_event.u.vec3.z = 9.8f + 0.15f * static_cast<float>(std::sin(angle * 0.5f));
            fake_event.u.vec3.status = ACCURACY_MEDIUM;
        } else {
            fake_event.sensorHandle = FAKE_ACCELEROMETER_HANDLE;
            fake_event.sensorType = SENSOR_TYPE_ACCELEROMETER;
            // fake_event.u.vec3.x = 0.0f;
            // fake_event.u.vec3.y = 0.0f;
            // fake_event.u.vec3.z = 9.8f;
            fake_event.u.vec3.status = ACCURACY_LOW;
        }
        fake_event.u.vec3.x = -accel_y;
        fake_event.u.vec3.y = accel_x;
        fake_event.u.vec3.z = accel_z;

        event_data = (sensors_event_t*) gbinder_writer_malloc0(&writer, sizeof(sensors_event_t));
        if (event_data) {
            *event_data = fake_event;
            emitted = 1;
        } else {
            err = RESULT_NO_MEMORY;
        }
    }

    gbinder_writer_append_int32(&writer, err);
    gbinder_writer_append_hidl_vec(&writer, event_data, emitted, sizeof(sensors_event_t));

    // Response includes an empty dynamic sensors vector per legacy behaviour
    gbinder_writer_append_hidl_vec(&writer, NULL, 0, sizeof(sensor_t));

    // GDEBUG("app_async_resp: emitted=%u err=%d flush=%d active=%d phase=%.2f",
        // emitted, err, fake_flush_pending ? 1 : 0, fake_sensor_activated ? 1 : 0, fake_sensor_phase);

    gbinder_remote_request_complete(resp->req, resp->reply, 0);
    return G_SOURCE_REMOVE;
}

static
void
app_async_free(
    gpointer user_data)
{
    Response* resp = (Response*)user_data;

    gbinder_local_reply_unref(resp->reply);
    gbinder_remote_request_unref(resp->req);
    g_free(resp);
}

static
GBinderLocalReply*
app_reply(
    GBinderLocalObject* obj,
    GBinderRemoteRequest* req,
    guint code,
    guint flags,
    int* status,
    void* user_data)
{
    App* app = (App*) user_data;
    GBinderLocalReply *reply = NULL;
    GBinderReader reader;
    GBinderWriter writer;

    // GINFO("app_reply: received transaction, code %u", code);

    gbinder_remote_request_init_reader(req, &reader);
    if (code == GET_SENSORS_LIST) {
        GINFO("Event: GET_SENSORS_LIST");
        const char* iface = gbinder_remote_request_interface(req);

        if (!g_strcmp0(iface, DEFAULT_IFACE)) {
            reply = gbinder_local_object_new_reply(obj);

            gbinder_local_reply_append_int32(reply, GBINDER_STATUS_OK);
            *status = GBINDER_STATUS_OK;

            // --- Create Fake Sensor List ---
            std::vector<sensor_t> sensors_vec;
            sensor_t fake_accelerometer;
            memset(&fake_accelerometer, 0, sizeof(fake_accelerometer));

            const char* name = "Fake Accelerometer";
            const char* vendor = "GitHub Copilot";
            const char* type_as_string = "android.sensor.accelerometer";
            const char* required_permission = "";

            fake_accelerometer.name.data.str = name;
            fake_accelerometer.name.len = strlen(name);
            fake_accelerometer.name.owns_buffer = TRUE;
            fake_accelerometer.vendor.data.str = vendor;
            fake_accelerometer.vendor.len = strlen(vendor);
            fake_accelerometer.vendor.owns_buffer = TRUE;
            fake_accelerometer.version = 1;
            fake_accelerometer.handle = FAKE_ACCELEROMETER_HANDLE;
            fake_accelerometer.type = SENSOR_TYPE_ACCELEROMETER;
            fake_accelerometer.typeAsString.data.str = type_as_string;
            fake_accelerometer.typeAsString.len = strlen(type_as_string);
            fake_accelerometer.typeAsString.owns_buffer = TRUE;
            fake_accelerometer.maxRange = 19.6f;
            fake_accelerometer.resolution = 0.01f;
            fake_accelerometer.power = 0.1f;
            fake_accelerometer.minDelay = 10000; // in microseconds
            fake_accelerometer.fifoReservedEventCount = 0;
            fake_accelerometer.fifoMaxEventCount = 0;
            fake_accelerometer.requiredPermission.data.str = required_permission;
            fake_accelerometer.requiredPermission.len = strlen(required_permission);
            fake_accelerometer.requiredPermission.owns_buffer = TRUE;
            fake_accelerometer.maxDelay = 500000;
            fake_accelerometer.flags = SENSOR_FLAG_CONTINUOUS_MODE;

            sensors_vec.push_back(fake_accelerometer);
            // --- End Fake Sensor List ---

            sensor_t* sensors;
            int sensors_len = sensors_vec.size();

            gbinder_local_reply_init_writer(reply, &writer);

            guint index;
            GBinderParent vec_parent;
            GBinderHidlVec *vec = gbinder_writer_new0(&writer, GBinderHidlVec);
            const gsize total = sensors_len * sizeof(*sensors);
            sensors = (sensor_t*) gbinder_writer_malloc0(&writer, total);

            /* Fill in the vector descriptor */
            if (sensors) {
                vec->data.ptr = sensors;
                vec->count = sensors_len;
            }
            vec->owns_buffer = TRUE;

            std::copy(sensors_vec.begin(), sensors_vec.end(), sensors);

            /* Prepare parent descriptor for the string data */
            vec_parent.index = gbinder_writer_append_buffer_object(&writer, vec, sizeof(*vec));
            vec_parent.offset = GBINDER_HIDL_VEC_BUFFER_OFFSET;

            index = gbinder_writer_append_buffer_object_with_parent(&writer,
                sensors, total, &vec_parent);

            for (int i = 0; i < sensors_len; i++)
                sensors_write_info_strings(&writer, sensors + i, index, i);
        } else {
            GDEBUG("Unexpected interface \"%s\"", iface);
        }
    } else if (code == SET_OPERATION_MODE) {
        GINFO("Event: SET_OPERATION_MODE");
        const char* iface = gbinder_remote_request_interface(req);

        if (!g_strcmp0(iface, DEFAULT_IFACE)) {
            reply = gbinder_local_object_new_reply(obj);

            gbinder_local_reply_append_int32(reply, GBINDER_STATUS_OK);
            *status = GBINDER_STATUS_OK;

            gbinder_local_reply_init_writer(reply, &writer);
            gbinder_writer_append_int32(&writer, RESULT_INVALID_OPERATION);
        } else {
            GDEBUG("Unexpected interface \"%s\"", iface);
        }
    } else if (code == ACTIVATE) {
        GINFO("Event: ACTIVATE");
        const char* iface = gbinder_remote_request_interface(req);

        if (!g_strcmp0(iface, DEFAULT_IFACE)) {
            int handle = 0;
            gboolean enabled;
            gbinder_reader_read_int32(&reader, &handle);
            gbinder_reader_read_bool(&reader, &enabled);

            if (handle == FAKE_ACCELEROMETER_HANDLE) {
                fake_sensor_activated = (enabled == TRUE);
                GINFO("Fake Accelerometer %s", fake_sensor_activated ? "activated" : "deactivated");
            }

            reply = gbinder_local_object_new_reply(obj);

            gbinder_local_reply_append_int32(reply, GBINDER_STATUS_OK);
            *status = GBINDER_STATUS_OK;

            gbinder_local_reply_init_writer(reply, &writer);
            gbinder_writer_append_int32(&writer, RESULT_OK);
        } else {
            GDEBUG("Unexpected interface \"%s\"", iface);
        }
    } else if (code == POLL) {
        // GINFO("Event: POLL");
        const char* iface = gbinder_remote_request_interface(req);

        if (!g_strcmp0(iface, DEFAULT_IFACE)) {
            int maxCount = 0;
            gbinder_reader_read_int32(&reader, &maxCount);

            reply = gbinder_local_object_new_reply(obj);

            gbinder_local_reply_append_int32(reply, GBINDER_STATUS_OK);
            *status = GBINDER_STATUS_OK;

            Response* resp = g_new0(Response, 1);
            resp->maxCount = maxCount;
            resp->reply = reply;
            resp->req = gbinder_remote_request_ref(req);
            g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, app_async_resp,
                                resp, app_async_free);
            gbinder_remote_request_block(resp->req);
            return NULL;
        } else {
            GDEBUG("Unexpected interface \"%s\"", iface);
        }
    } else if (code == BATCH) {
        GINFO("Event: BATCH");
        const char* iface = gbinder_remote_request_interface(req);

        if (!g_strcmp0(iface, DEFAULT_IFACE)) {
            gint32 tmp = 0;
            gint64 tmp64 = 0;
            gbinder_reader_read_int32(&reader, &tmp);
            gbinder_reader_read_int64(&reader, &tmp64);
            gbinder_reader_read_int64(&reader, &tmp64);

            reply = gbinder_local_object_new_reply(obj);

            gbinder_local_reply_append_int32(reply, GBINDER_STATUS_OK);
            *status = GBINDER_STATUS_OK;

            gbinder_local_reply_init_writer(reply, &writer);
            gbinder_writer_append_int32(&writer, RESULT_OK);
        } else {
            GDEBUG("Unexpected interface \"%s\"", iface);
        }
    } else if (code == FLUSH) {
        GINFO("Event: FLUSH");
        const char* iface = gbinder_remote_request_interface(req);

        if (!g_strcmp0(iface, DEFAULT_IFACE)) {
            int handle = 0;
            gbinder_reader_read_int32(&reader, &handle);

            reply = gbinder_local_object_new_reply(obj);

            gbinder_local_reply_append_int32(reply, GBINDER_STATUS_OK);
            *status = GBINDER_STATUS_OK;

            gbinder_local_reply_init_writer(reply, &writer);
            if (handle == FAKE_ACCELEROMETER_HANDLE) {
                fake_flush_pending = true;
                fake_flush_handle = handle;
                GDEBUG("Queued META_DATA flush complete for handle %d", handle);
            }
            gbinder_writer_append_int32(&writer, RESULT_OK);
        } else {
            GDEBUG("Unexpected interface \"%s\"", iface);
        }
    } else if (code == INJECT_SENSOR_DATA) {
        GINFO("Event: INJECT_SENSOR_DATA");
        const char* iface = gbinder_remote_request_interface(req);

        if (!g_strcmp0(iface, DEFAULT_IFACE)) {
            reply = gbinder_local_object_new_reply(obj);

            gbinder_local_reply_append_int32(reply, GBINDER_STATUS_OK);
            *status = GBINDER_STATUS_OK;

            gbinder_local_reply_init_writer(reply, &writer);
            gbinder_writer_append_int32(&writer, RESULT_INVALID_OPERATION);
        } else {
            GDEBUG("Unexpected interface \"%s\"", iface);
        }
    } else if (code == REGISTER_DIRECT_CHANNEL) {
        GINFO("Event: REGISTER_DIRECT_CHANNEL");
        const char* iface = gbinder_remote_request_interface(req);

        if (!g_strcmp0(iface, DEFAULT_IFACE)) {
            reply = gbinder_local_object_new_reply(obj);

            gbinder_local_reply_append_int32(reply, GBINDER_STATUS_OK);
            *status = GBINDER_STATUS_OK;

            gbinder_local_reply_init_writer(reply, &writer);
            gbinder_writer_append_int32(&writer, RESULT_INVALID_OPERATION);
            gbinder_writer_append_int32(&writer, -1);
        } else {
            GDEBUG("Unexpected interface \"%s\"", iface);
        }
    } else if (code == UNREGISTER_DIRECT_CHANNEL) {
        GINFO("Event: UNREGISTER_DIRECT_CHANNEL");
        const char* iface = gbinder_remote_request_interface(req);

        if (!g_strcmp0(iface, DEFAULT_IFACE)) {
            int tmp = 0;
            gbinder_reader_read_int32(&reader, &tmp);

            reply = gbinder_local_object_new_reply(obj);

            gbinder_local_reply_append_int32(reply, GBINDER_STATUS_OK);
            *status = GBINDER_STATUS_OK;

            gbinder_local_reply_init_writer(reply, &writer);
            gbinder_writer_append_int32(&writer, RESULT_OK);
        } else {
            GDEBUG("Unexpected interface \"%s\"", iface);
        }
    } else if (code == CONFIG_DIRECT_REPORT) {
        GINFO("Event: CONFIG_DIRECT_REPORT");
        const char* iface = gbinder_remote_request_interface(req);

        if (!g_strcmp0(iface, DEFAULT_IFACE)) {
            reply = gbinder_local_object_new_reply(obj);

            gbinder_local_reply_append_int32(reply, GBINDER_STATUS_OK);
            *status = GBINDER_STATUS_OK;

            gbinder_local_reply_init_writer(reply, &writer);
            gbinder_writer_append_int32(&writer, RESULT_INVALID_OPERATION);
            gbinder_writer_append_int32(&writer, -1);
        } else {
            GDEBUG("Unexpected interface \"%s\"", iface);
        }
    }

    return reply;
}

static
void
app_add_service_done(
    GBinderServiceManager* sm,
    int status,
    void* user_data)
{
    App* app = (App*) user_data;

    if (status == GBINDER_STATUS_OK) {
        printf("Added \"%s\"\n", DEFAULT_NAME);
        app->ret = RET_OK;
    } else {
        GERR("Failed to add \"%s\" (%d)", DEFAULT_NAME, status);
        g_main_loop_quit(app->loop);
    }
}

static
void
app_sm_presence_handler(
    GBinderServiceManager* sm,
    void* user_data)
{
    App* app = (App*) user_data;

    if (gbinder_servicemanager_is_present(app->sm)) {
        GINFO("Service manager has reappeared");
        gbinder_servicemanager_add_service(app->sm, DEFAULT_NAME, app->obj,
            app_add_service_done, app);
    } else {
        GINFO("Service manager has died");
    }
}

static
void
app_run(
   App* app)
{
        GError *error = NULL;

    const char* name = DEFAULT_NAME;
    guint sigtrm = g_unix_signal_add(SIGTERM, app_signal, app);
    guint sigint = g_unix_signal_add(SIGINT, app_signal, app);
    gulong presence_id = gbinder_servicemanager_add_presence_handler
        (app->sm, app_sm_presence_handler, app);

    app->loop = g_main_loop_new(NULL, TRUE);
    SSCSensorAccelerometer *sensor = ssc_sensor_accelerometer_new_sync(NULL, &error);
    if (!sensor) {
        g_printerr("Nie mogę utworzyć sensora SSC: %s\n", error ? error->message : "unknown");
        g_clear_error(&error);
        g_main_loop_unref(app->loop);
        return;
    }

    g_signal_connect(sensor, "measurement", G_CALLBACK(measurement_cb), NULL);

    if (!ssc_sensor_accelerometer_open_sync(sensor, NULL, &error)) {
        g_printerr("Nie mogę otworzyć sensora SSC: %s\n", error ? error->message : "unknown");
        g_clear_error(&error);
        g_object_unref(sensor);
        g_main_loop_unref(app->loop);
        return;
    }
    g_unix_signal_add(SIGINT, stop_requested, sensor);
    g_unix_signal_add(SIGTERM, stop_requested, sensor);

    gbinder_servicemanager_add_service(app->sm, DEFAULT_NAME, app->obj,
        app_add_service_done, app);

    GINFO("Waydroid Sensors HAL service ready.");

    g_main_loop_run(app->loop);

    if (sigtrm) g_source_remove(sigtrm);
    if (sigint) g_source_remove(sigint);
    gbinder_servicemanager_remove_handler(app->sm, presence_id);
    g_main_loop_unref(app->loop);
    app->loop = NULL;
}





int main(int argc, char* argv[])
{


    const char* device;
    App app;

    srand(time(NULL)); // Initialize random seed

    gutil_log_timestamp = FALSE;
    gutil_log_set_type(GLOG_TYPE_STDERR, logtag);
    gutil_log_default.level = GLOG_LEVEL_VERBOSE;

    if (argc < 2)
        device = DEFAULT_DEVICE;
    else
        device = argv[1];

    memset(&app, 0, sizeof(app));
    app.ret = RET_INVARG;

    app.sm = gbinder_servicemanager_new2(device, "hidl", "hidl");
    if (gbinder_servicemanager_wait(app.sm, -1)) {
        app.obj = gbinder_servicemanager_new_local_object
            (app.sm, DEFAULT_IFACE, app_reply, &app);
        app_run(&app);
        gbinder_local_object_unref(app.obj);
        gbinder_servicemanager_unref(app.sm);
    }
    return app.ret;
}
