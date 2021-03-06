#include "websocket_server.h"
#include "core/os/os.h"

void WebSocketServer::_bind_methods() {

	ClassDB::bind_method(D_METHOD("is_listening"), &WebSocketServer::is_listening);
	ClassDB::bind_method(D_METHOD("listen", "port", "protocols"), &WebSocketServer::listen, DEFVAL(PoolVector<String>()));
	ClassDB::bind_method(D_METHOD("stop"), &WebSocketServer::stop);
	ClassDB::bind_method(D_METHOD("has_peer", "id"), &WebSocketServer::has_peer);
	ClassDB::bind_method(D_METHOD("get_stream_peer", "id"), &WebSocketServer::get_stream_peer);
	//BIND_ENUM_CONSTANT(COMPRESS_NONE);

	ADD_SIGNAL(MethodInfo("client_disconnected", PropertyInfo(Variant::INT, "id")));
	ADD_SIGNAL(MethodInfo("client_connected", PropertyInfo(Variant::INT, "id"), PropertyInfo(Variant::STRING, "protocol")));
	ADD_SIGNAL(MethodInfo("data_received", PropertyInfo(Variant::INT, "id")));
}

uint32_t WebSocketServer::_gen_unique_id() const {

	uint32_t hash = 0;

	while (hash == 0 || hash == 1) {

		hash = hash_djb2_one_32(
				(uint32_t)OS::get_singleton()->get_ticks_usec());
		hash = hash_djb2_one_32(
				(uint32_t)OS::get_singleton()->get_unix_time(), hash);
		hash = hash_djb2_one_32(
				(uint32_t)OS::get_singleton()->get_data_dir().hash64(), hash);
		/*
		hash = hash_djb2_one_32(
			(uint32_t)OS::get_singleton()->get_unique_id().hash64(), hash );
		*/
		hash = hash_djb2_one_32(
				(uint32_t)((uint64_t)this), hash); //rely on aslr heap
		hash = hash_djb2_one_32(
				(uint32_t)((uint64_t)&hash), hash); //rely on aslr stack

		hash = hash & 0x7FFFFFFF; // make it compatible with unsigned, since negatie id is used for exclusion
	}

	return hash;
}

Error WebSocketServer::listen(int p_port, PoolVector<String> p_protocols) {

	struct lws_context_creation_info info;
	memset(&info, 0, sizeof info);

	stop();

	if (p_protocols.size() == 0) // default to binary protocol
		p_protocols.append(String("binary"));

	// Prepare lws protocol structs
	_make_protocols(p_protocols);
	PoolVector<struct lws_protocols>::Read pr = protocol_structs.read();

	info.port = p_port;
	info.user = this;
	info.protocols = &pr[0];
	info.gid = -1;
	info.uid = -1;
	//info.ws_ping_pong_interval = 5;

	context = lws_create_context(&info);

	if (context != NULL)
		return OK;

	return FAILED;
}

bool WebSocketServer::is_listening() const {
	return context != NULL;
}

int WebSocketServer::_handle_cb(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {

	WebSocketPeer::PeerData *peer_data = (WebSocketPeer::PeerData *)user;

	switch (reason) {
		case LWS_CALLBACK_HTTP:
			// no http for now
			// closing immediately returning 1;
			return 1;

		case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
			// check header here?
			break;

		case LWS_CALLBACK_ESTABLISHED: {
			int id = _gen_unique_id();

			Ref<WebSocketPeer> peer = Ref<WebSocketPeer>(memnew(WebSocketPeer));
			peer->set_wsi(wsi);
			peer_map[id] = peer;

			peer_data->peer_id = id;
			peer_data->rbw.resize(16);
			peer_data->rbr.resize(16);
			peer_data->force_close = false;

			emit_signal("client_connected", peer_data->peer_id, lws_get_protocol(wsi)->name);
			break;
		}

		case LWS_CALLBACK_CLOSED: {
			int id = peer_data->peer_id;
			if (peer_map.has(id)) {
				peer_map[id]->close();
				peer_map.erase(id);
			}
			peer_data->rbr.resize(0);
			peer_data->rbw.resize(0);
			emit_signal("client_disconnected", id);
			return 0; // we can end here
		}

		case LWS_CALLBACK_RECEIVE: {
			int id = peer_data->peer_id;
			if (peer_map.has(id))
				peer_map[id]->read_wsi(in, len);
			emit_signal("data_received", id);
			break;
		}

		case LWS_CALLBACK_SERVER_WRITEABLE: {
			if (peer_data->force_close)
				return -1;

			int id = peer_data->peer_id;
			if (peer_map.has(id))
				peer_map[id]->write_wsi();
			break;
		}

		default:
			break;
	}

	return 0;
}

void WebSocketServer::stop() {
	if (context == NULL)
		return;

	peer_map.clear();
	destroy_context();
	context = NULL;
}

bool WebSocketServer::has_peer(int p_id) {
	return peer_map.has(p_id);
}

Ref<StreamPeer> WebSocketServer::get_stream_peer(int p_id) {
	ERR_FAIL_COND_V(!has_peer(p_id), NULL);
	return peer_map[p_id];
}

WebSocketServer::WebSocketServer() {
	context = NULL;
	free_context = false;
	is_polling = false;
}

WebSocketServer::~WebSocketServer() {
	stop();
}
