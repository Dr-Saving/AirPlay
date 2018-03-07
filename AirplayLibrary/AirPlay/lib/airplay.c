#define _CRT_SECURE_NO_WARNINGS
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "airplay.h"
#include "mycrypt.h"
#include "raop_rtp.h"
#include "digest.h"
#include "httpd.h"	
#include "sdp.h"
#include "global.h"
#include "utils.h"
#include "netutils.h"
#include "logger.h"
#include "compat.h"
//to do fairplay
//#include "li"

#define MAX_SIGNATURE_LEN 512

#define MAX_PASSWORD_LEN 64

/* MD5 as hex fits here */
#define MAX_NONCE_LEN 32

#define MAX_PACKET_LEN 4096

struct airplay_conn_s {
	airplay_t *airplay;
	raop_rtp_t *airplay_rtp;

	unsigned char *local;
	int locallen;

	unsigned char *remote;
	int remotelen;

	char nonce[MAX_NONCE_LEN + 1];

	unsigned char aeskey[16];
	unsigned char iv[16];
	unsigned char buffer[MAX_PACKET_LEN];
	int pos;
};
typedef struct airplay_conn_s airplay_conn_t;

#define RECEIVEBUFFER 1024

#define AIRPLAY_STATUS_OK                  200
#define AIRPLAY_STATUS_SWITCHING_PROTOCOLS 101
#define AIRPLAY_STATUS_NEED_AUTH           401
#define AIRPLAY_STATUS_NOT_FOUND           404
#define AIRPLAY_STATUS_METHOD_NOT_ALLOWED  405
#define AIRPLAY_STATUS_PRECONDITION_FAILED 412
#define AIRPLAY_STATUS_NOT_IMPLEMENTED     501
#define AIRPLAY_STATUS_NO_RESPONSE_NEEDED  1000

#define EVENT_NONE     -1
#define EVENT_PLAYING   0
#define EVENT_PAUSED    1
#define EVENT_LOADING   2
#define EVENT_STOPPED   3

const char *eventStrings[] = { "playing", "paused", "loading", "stopped" };

#define STREAM_INFO  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"\
"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\r\n"\
"<plist version=\"1.0\">\r\n"\
"<dict>\r\n"\
"<key>width</key>\r\n"\
"<integer>1280</integer>\r\n"\
"<key>height</key>\r\n"\
"<integer>720</integer>\r\n"\
"<key>version</key>\r\n"\
"<string>110.92</string>\r\n"\
"</dict>\r\n"\
"</plist>\r\n"

#define PLAYBACK_INFO  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"\
"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\r\n"\
"<plist version=\"1.0\">\r\n"\
"<dict>\r\n"\
"<key>duration</key>\r\n"\
"<real>%f</real>\r\n"\
"<key>loadedTimeRanges</key>\r\n"\
"<array>\r\n"\
"\t\t<dict>\r\n"\
"\t\t\t<key>duration</key>\r\n"\
"\t\t\t<real>%f</real>\r\n"\
"\t\t\t<key>start</key>\r\n"\
"\t\t\t<real>0.0</real>\r\n"\
"\t\t</dict>\r\n"\
"</array>\r\n"\
"<key>playbackBufferEmpty</key>\r\n"\
"<true/>\r\n"\
"<key>playbackBufferFull</key>\r\n"\
"<false/>\r\n"\
"<key>playbackLikelyToKeepUp</key>\r\n"\
"<true/>\r\n"\
"<key>position</key>\r\n"\
"<real>%f</real>\r\n"\
"<key>rate</key>\r\n"\
"<real>%d</real>\r\n"\
"<key>readyToPlay</key>\r\n"\
"<true/>\r\n"\
"<key>seekableTimeRanges</key>\r\n"\
"<array>\r\n"\
"\t\t<dict>\r\n"\
"\t\t\t<key>duration</key>\r\n"\
"\t\t\t<real>%f</real>\r\n"\
"\t\t\t<key>start</key>\r\n"\
"\t\t\t<real>0.0</real>\r\n"\
"\t\t</dict>\r\n"\
"</array>\r\n"\
"</dict>\r\n"\
"</plist>\r\n"

#define PLAYBACK_INFO_NOT_READY  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"\
"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\r\n"\
"<plist version=\"1.0\">\r\n"\
"<dict>\r\n"\
"<key>readyToPlay</key>\r\n"\
"<false/>\r\n"\
"</dict>\r\n"\
"</plist>\r\n"

#define SERVER_INFO  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"\
"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\r\n"\
"<plist version=\"1.0\">\r\n"\
"<dict>\r\n"\
"<key>deviceid</key>\r\n"\
"<string>%s</string>\r\n"\
"<key>features</key>\r\n"\
"<integer>119</integer>\r\n"\
"<key>model</key>\r\n"\
"<string>Kodi,1</string>\r\n"\
"<key>protovers</key>\r\n"\
"<string>1.0</string>\r\n"\
"<key>srcvers</key>\r\n"\
"<string>"AIRPLAY_SERVER_VERSION_STR"</string>\r\n"\
"</dict>\r\n"\
"</plist>\r\n"

#define EVENT_INFO "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\r\n"\
"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n\r\n"\
"<plist version=\"1.0\">\r\n"\
"<dict>\r\n"\
"<key>category</key>\r\n"\
"<string>video</string>\r\n"\
"<key>sessionID</key>\r\n"\
"<integer>%d</integer>\r\n"\
"<key>state</key>\r\n"\
"<string>%s</string>\r\n"\
"</dict>\r\n"\
"</plist>\r\n"\

#define AUTH_REALM "AirPlay"
#define AUTH_REQUIRED "WWW-Authenticate: Digest realm=\""  AUTH_REALM  "\", nonce=\"%s\"\r\n"

static void *
conn_init(void *opaque, unsigned char *local, int locallen, unsigned char *remote, int remotelen)
{
	airplay_conn_t *conn;

	conn = calloc(1, sizeof(airplay_conn_t));
	if (!conn) {
		return NULL;
	}
	conn->airplay = opaque;
	conn->airplay_rtp = NULL;

	if (locallen == 4) {
		logger_log(conn->airplay->logger, LOGGER_INFO,
			"Local: %d.%d.%d.%d",
			local[0], local[1], local[2], local[3]);
	} else if (locallen == 16) {
		logger_log(conn->airplay->logger, LOGGER_INFO,
			"Local: %02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
			local[0], local[1], local[2], local[3], local[4], local[5], local[6], local[7],
			local[8], local[9], local[10], local[11], local[12], local[13], local[14], local[15]);
	}
	if (remotelen == 4) {
		logger_log(conn->airplay->logger, LOGGER_INFO,
			"Remote: %d.%d.%d.%d",
			remote[0], remote[1], remote[2], remote[3]);
	} else if (remotelen == 16) {
		logger_log(conn->airplay->logger, LOGGER_INFO,
			"Remote: %02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
			remote[0], remote[1], remote[2], remote[3], remote[4], remote[5], remote[6], remote[7],
			remote[8], remote[9], remote[10], remote[11], remote[12], remote[13], remote[14], remote[15]);
	}

	conn->local = malloc(locallen);
	assert(conn->local);
	memcpy(conn->local, local, locallen);

	conn->remote = malloc(remotelen);
	assert(conn->remote);
	memcpy(conn->remote, remote, remotelen);

	conn->locallen = locallen;
	conn->remotelen = remotelen;

	digest_generate_nonce(conn->nonce, sizeof(conn->nonce));
	return conn;
}

static void 
conn_request(void *ptr, http_request_t *request, http_response_t **response)
{
	const char realm[] = "airplay";
	airplay_conn_t *conn = ptr;
	airplay_t *airplay = conn->airplay;

	const char *cseq;
	const char *challenge;
	int require_auth = 0;
	char responseHeader[4096];
	char responseBody[4096];
	int responseLength = 0;

	const char *uri = http_request_get_url(request);
	const char *method = http_request_get_method(request);

	const char * contentType = http_request_get_header(request, "content-type");
	const char * m_sessionId = http_request_get_header(request, "x-apple-session-id");
	const char * authorization = http_request_get_header(request, "authorization");
	const char * photoAction = http_request_get_header(request, "x-apple-assetaction");
	const char * photoCacheId = http_request_get_header(request, "x-apple-assetkey");

	int status = AIRPLAY_STATUS_OK;
	int needAuth = 0;

	if (!method) {
		return;
	}

	logger_log(conn->airplay->logger, LOGGER_DEBUG, "%s uri=%s\n", method, uri);

	{
		const char *data;
		int len;
		data = http_request_get_data(request, &len);
		logger_log(conn->airplay->logger, LOGGER_DEBUG, "data len %d:%s\n", len, data);
	}

	


}

static void 
conn_destroy(void *ptr)
{
	airplay_conn_t *conn = ptr;
	if (conn->airplay_rtp) {
		raop_rtp_destroy(conn->airplay_rtp);
	}
	free(conn->local);
	free(conn->remote);
	free(conn);
}

static void 
conn_datafeed(void *ptr, unsigned char *data, int len)
{
	int size;
	unsigned short type;
	unsigned short type1;

	airplay_conn_t *conn = ptr;
	size = *(int*)data;
	type = *(unsigned short*)(data + 4);
	type1 = *(unsigned short*)(data + 6);

	logger_log(conn->airplay->logger, LOGGER_DEBUG, "Add data size=%d type %2x %2x", size, type, type1);
}

airplay_t *
airplay_init(int max_clients, raop_callbacks_t *callbacks, const char *pemkey, int *error)
{
	airplay_t *airplay;
	httpd_t *httpd;
	rsakey_t *rsakey;
	httpd_callbacks_t httpd_cbs;

	assert(callbacks);
	assert(max_clients > 0);
	assert(max_clients < 100);
	assert(pemkey);

	if (netutils_init() < 0) {
		return NULL;
	}

	if (!callbacks->audio_init||
		!callbacks->audio_process||
		!callbacks->audio_destroy) 
    {
		return NULL;
	}

	airplay = calloc(1, sizeof(airplay_t));
	if (!airplay) {
		return NULL;
	}

	airplay->logger = logger_init();

	memset(&httpd_cbs, 0, sizeof(httpd_cbs));
	httpd_cbs.opaque = airplay;
	httpd_cbs.conn_init = &conn_init;
	httpd_cbs.conn_request = &conn_request;
	httpd_cbs.conn_destroy = &conn_destroy;
	httpd_cbs.conn_datafeed = &conn_datafeed;

	httpd = httpd_init(airplay->logger, &httpd_cbs, max_clients, 0);
	if (!httpd) {
		free(airplay);
		return NULL;
	}
	airplay->main_server = httpd;

	httpd = httpd_init(airplay->logger, &httpd_cbs, max_clients, 1);
	if (!httpd) {
		free(airplay->main_server);
		free(airplay);
		return NULL;
	}
	airplay->mirror_server = httpd;

	httpd = httpd_init(airplay->logger, &httpd_cbs, max_clients, 2);
	if (!httpd) {
		free(airplay->mirror_server);
		free(airplay->main_server);
		free(airplay);
		return NULL;
	}
	airplay->event_server = httpd;

	airplay->es1 = httpd_init(airplay->logger, &httpd_cbs, max_clients, 3);
	airplay->es2 = httpd_init(airplay->logger, &httpd_cbs, max_clients, 4);
	airplay->es3 = httpd_init(airplay->logger, &httpd_cbs, max_clients, 5);

	memcpy(&airplay->callbacks, callbacks, sizeof(raop_callbacks_t));

	/* Initialize RSA key handler */
	rsakey = rsakey_init_pem(pemkey);
	if (!rsakey) {
		free(airplay->event_server);
		free(airplay->mirror_server);
		free(airplay->main_server);
		free(airplay);
		return NULL;
	}

	airplay->rsakey = rsakey;
	return airplay;
}

airplay_t *
airplay_init_from_keyfile(int max_clients, raop_callbacks_t *callbacks, const char *keyfile, int *error)
{
	airplay_t *airplay;
	char *pemstr;

	if (utils_read_file(&pemstr,keyfile) < 0) {
		return NULL;
	}
	airplay = airplay_init(max_clients, callbacks, pemstr, error);
	free(pemstr);
	return airplay;
}

void airpaly_destroy(airplay_t *airplay)
{
	if (airplay) {
		airplay_stop(airplay);
		httpd_destroy(airplay->main_server);
		httpd_destroy(airplay->mirror_server);
		httpd_destroy(airplay->event_server);
		httpd_destroy(airplay->es1);
		httpd_destroy(airplay->es2);
		httpd_destroy(airplay->es3);
		rsakey_destroy(airplay->rsakey);
		logger_destroy(airplay->logger);
		free(airplay);
		netutils_cleanup();
	}
}

int
airplay_is_running(airplay_t *airplay)
{
	assert(airplay);
	return httpd_is_running(airplay->main_server);
}

void
airplay_set_log_level(airplay_t *airplay, int level)
{
	assert(airplay);
	logger_set_level(airplay->logger, level);
}

void
airplay_set_log_callback(airplay_t *airplay, airplay_log_callback_t callback, void *cls)
{
	assert(airplay);
	logger_set_callback(airplay->logger, callback, cls);
}

int airplay_start(airplay_t *airplay, unsigned short *port, const char *hwaddr, int hwaddrlen, const char *password)
{
	int ret;
	unsigned short mirror_port = 7100, event_port = 55556, ep1, ep2, ep3;
	assert(airplay);
	assert(port);
	assert(hwaddr);

	if (g_port_seted) {
		event_port = 55557;
		g_event_port = 55557;
		g_port_seted = 0;
	} else {
		event_port = 55556;
		g_event_port = 55556;
		g_port_seted = 1;
	}
	ep1 = 55558;
	ep2 = 55559;
	ep3 = 55560;

	if (hwaddrlen > MAX_HWADDR_LEN) {
		return -1;
	}

	memset(airplay->password, 0, sizeof(airplay->password));
	if (password) {
		if (strlen(password) > MAX_PASSWORD_LEN) {
			return -1;
		}
		strncpy(airplay->password, password, MAX_PASSWORD_LEN);
	}

	memcpy(airplay->hwaddr, hwaddr, hwaddrlen);
	airplay->hwaddrlen = hwaddrlen;

	ret = httpd_start(airplay->mirror_server, &mirror_port);
	if (ret != 1) return ret;

	//ret = httpd_start(airplay->event_server, &event_port);
	//if (ret != 1) return ret;

	//ret = httpd_start(airplay->es1, &ep1);
	//if (ret != 1) return ret;

	//ret = httpd_start(airplay->es2, &ep2);
	//if (ret != 1) return ret;

	//ret = httpd_start(airplay->es3, &ep3);
	//if (ret != 1) return ret;

	return 1;//httpd_start(airplay->main_server, port);
}

void airplay_stop(airplay_t *airplay)
{
	assert(airplay);
	httpd_stop(airplay->main_server);
	httpd_stop(airplay->mirror_server);
	httpd_stop(airplay->event_server);
}