// List of sessions
Janus.sessions = {};

// Initialization
Janus.init = function(options) {
	if(Janus.initDone === undefined) {
		options = options || {};
		options.callback = (typeof options.callback == "function") ? options.callback : jQuery.noop;
		if(typeof console == "undefined" || typeof console.log == "undefined")
			console = { log: function() {} };
		// Console log (debugging disabled by default)
		Janus.log = (options.debug === true) ? console.log.bind(console) : jQuery.noop;
		Janus.log("Initializing library");
		Janus.initDone = true;
		// Detect tab close
		window.onbeforeunload = function() {
			Janus.log("Closing window");
			for(var s in Janus.sessions) {
				Janus.log("Destroying session " + s);
				Janus.sessions[s].destroy();
			}
		}
		// Helper to add external JavaScript sources
		function addJs(src) {
			if(src === 'jquery.min.js') {
				if(window.jQuery) {
					// Already loaded
					options.callback();
					return;
				}
			}
			var oHead = document.getElementsByTagName('head').item(0);
			var oScript = document.createElement("script");
			oScript.type = "text/javascript";
			oScript.src = src;
			oScript.onload = function() {
				Janus.log("Library " + src + " loaded");
				if(src === 'jquery.min.js') {
					options.callback();
				}
			}
			oHead.appendChild( oScript);
		};

		addJs('adapter.js');
		addJs('jquery.min.js');
	}
};

// Helper method to check whether WebRTC is supported by this browser
Janus.isWebrtcSupported = function() {
	if(RTCPeerConnection === null || getUserMedia === null) {
		return false;
	}
	return true;
};

function Janus(gatewayCallbacks) {
	if(!Janus.isWebrtcSupported()) {
		gatewayCallbacks.error("WebRTC not supported by this browser");
		return {};
	}
	if(Janus.initDone === undefined) {
		gatewayCallbacks.error("Library not initialized");
		return {};
	}
	Janus.log("Library initialized: " + Janus.initDone);
	gatewayCallbacks = gatewayCallbacks || {};
	gatewayCallbacks.success = (typeof gatewayCallbacks.success == "function") ? gatewayCallbacks.success : jQuery.noop;
	gatewayCallbacks.error = (typeof gatewayCallbacks.error == "function") ? gatewayCallbacks.error : jQuery.noop;
	gatewayCallbacks.destroyed = (typeof gatewayCallbacks.destroyed == "function") ? gatewayCallbacks.destroyed : jQuery.noop;
	if(gatewayCallbacks.server === null || gatewayCallbacks.server === undefined) {
		gatewayCallbacks.error("Invalid gateway url");
		return {};
	}
	var iceServers = gatewayCallbacks.iceServers;
	if(iceServers === undefined || iceServers === null)
		iceServers = [{"url": "stun:stun.l.google.com:19302"}];
	var server = gatewayCallbacks.server;
	var connected = false;
	var sessionId = null;
	var pluginHandles = {};
	var that = this;
	var retries = 0;
	createSession(gatewayCallbacks);

	// Public methods
	this.getServer = function() { return server; };
	this.isConnected = function() { return connected; };
	this.getSessionId = function() { return sessionId; };
	this.destroy = function(callbacks) { destroySession(callbacks); };
	this.attach = function(callbacks) { createHandle(callbacks); };
	
	// Private method to create random identifiers (e.g., transaction)
	function randomString(len) {
		charSet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
		var randomString = '';
		for (var i = 0; i < len; i++) {
			var randomPoz = Math.floor(Math.random() * charSet.length);
			randomString += charSet.substring(randomPoz,randomPoz+1);
		}
		return randomString;
	}

	function eventHandler() {
		if(sessionId == null)
			return;
		Janus.log('Long poll...');
		if(!connected) {
			Janus.log("Is the gateway down? (connected=false)");
			return;
		}
		$.ajax({
			type: 'GET',
			url: server + "/" + sessionId + "?rid=" + new Date().getTime(),
			cache: false,
			timeout: 60000,	// FIXME
			success: handleEvent,
			error: function(XMLHttpRequest, textStatus, errorThrown) {
				Janus.log(textStatus + ": " + errorThrown);
				//~ clearTimeout(timeoutTimer);
				retries++;
				if(retries > 3) {
					// Did we just lost the gateway? :-(
					connected = false;
					gatewayCallbacks.error("Lost connection to the gateway (is it down?)");
					return;
				}
				eventHandler();
			},
			dataType: "json"
		});
	}
	
	// Private event handler: this will trigger plugin callbacks, if set
	function handleEvent(json) {
		retries = 0;
		if(sessionId !== undefined && sessionId !== null)
			setTimeout(eventHandler, 200);
		Janus.log("Got event on session " + sessionId);
		Janus.log(json);
		if(json["janus"] === "keepalive") {
			// Nothing happened
			return;
		} else if(json["janus"] === "event") {
			var sender = json["sender"];
			if(sender === undefined || sender === null) {
				Janus.log("Missing sender...");
				return;
			}
			var plugindata = json["plugindata"];
			if(plugindata === undefined || plugindata === null) {
				Janus.log("Missing plugindata...");
				return;
			}
			Janus.log("  -- Event is coming from " + sender + " (" + plugindata["plugin"] + ")");
			var data = plugindata["data"];
			Janus.log(data);
			var pluginHandle = pluginHandles[sender];
			if(pluginHandle === undefined || pluginHandle === null) {
				Janus.log("This handle is not attached to this session");
				return;
			}
			var jsep = json["jsep"];
			if(jsep !== undefined && jsep !== null) {
				Janus.log("Handling SDP as well...");
				Janus.log(jsep);
			}
			var callback = pluginHandle.onmessage;
			if(callback !== null && callback !== undefined) {
				Janus.log("Notifying application...");
				// Send to callback specified when attaching plugin handle
				callback(data, jsep);
			} else {
				// Send to generic callback (?)
				Janus.log("No provided notification callback");
			}
		} else {
			Janus.log("Unknown message '" + json["janus"] + "'");
		}
	}

	// Private method to create a session
	function createSession(callbacks) {
		var request = { "janus": "create", "transaction": randomString(12) };
		$.ajax({
			type: 'POST',
			url: server,
			cache: false,
			contentType: "application/json",
			data: JSON.stringify(request),
			success: function(json) {
				Janus.log("Create session:");
				Janus.log(json);
				if(json["janus"] !== "success") {
					Janus.log("Ooops: " + json["error"].code + " " + json["error"].reason);	// FIXME
					callbacks.error(json["error"].reason);
					return;
				}
				connected = true;
				sessionId = json.data["id"];
				Janus.log("Created session: " + sessionId);
				Janus.sessions[sessionId] = that;
				eventHandler();
				callbacks.success();
			},
			error: function(XMLHttpRequest, textStatus, errorThrown) {
				Janus.log(textStatus + ": " + errorThrown);	// FIXME
				if(errorThrown === "")
					callbacks.error(textStatus + ": Is the gateway down?");
				else
					callbacks.error(textStatus + ": " + errorThrown);
			},
			dataType: "json"
		});
	}

	// Private method to destroy a session
	function destroySession(callbacks, syncRequest) {
		syncRequest = (syncRequest === true);
		Janus.log("Destroying session " + sessionId + "(sync=" + syncRequest + ")");
		callbacks = callbacks || {};
		// FIXME This method triggers a success even when we fail
		callbacks.success = (typeof callbacks.success == "function") ? callbacks.success : jQuery.noop;
		if(!connected) {
			Janus.log("Is the gateway down? (connected=false)");
			callbacks.success();
			return;
		}
		if(sessionId === undefined || sessionId === null) {
			Janus.log("No session to destroy");
			callbacks.success();
			gatewayCallbacks.destroyed();
			return;
		}
		delete Janus.sessions[sessionId];
		// Destroy all handles first
		for(ph in pluginHandles) {
			var phv = pluginHandles[ph];
			Janus.log("Destroying handle " + phv.id + " (" + phv.plugin + ")");
			destroyHandle(phv.id, null, syncRequest);
		}
		// Ok, go on
		var request = { "janus": "destroy", "transaction": randomString(12) };
		$.ajax({
			type: 'POST',
			url: server + "/" + sessionId,
			async: syncRequest,	// Sometimes we need false here, or destroying in onbeforeunload won't work
			cache: false,
			contentType: "application/json",
			data: JSON.stringify(request),
			success: function(json) {
				Janus.log("Destroyed session:");
				Janus.log(json);
				sessionId = null;
				connected = false;
				if(json["janus"] !== "success") {
					Janus.log("Ooops: " + json["error"].code + " " + json["error"].reason);	// FIXME
				}
				callbacks.success();
				gatewayCallbacks.destroyed();
			},
			error: function(XMLHttpRequest, textStatus, errorThrown) {
				Janus.log(textStatus + ": " + errorThrown);	// FIXME
				// Reset everything anyway
				sessionId = null;
				connected = false;
				callbacks.success();
				gatewayCallbacks.destroyed();
			},
			dataType: "json"
		});
	}
	
	// Private method to create a plugin handle
	function createHandle(callbacks) {
		callbacks = callbacks || {};
		callbacks.success = (typeof callbacks.success == "function") ? callbacks.success : jQuery.noop;
		callbacks.error = (typeof callbacks.error == "function") ? callbacks.error : jQuery.noop;
		callbacks.consentDialog = (typeof callbacks.consentDialog == "function") ? callbacks.consentDialog : jQuery.noop;
		callbacks.onmessage = (typeof callbacks.onmessage == "function") ? callbacks.onmessage : jQuery.noop;
		callbacks.onlocalstream = (typeof callbacks.onlocalstream == "function") ? callbacks.onlocalstream : jQuery.noop;
		callbacks.onremotestream = (typeof callbacks.onremotestream == "function") ? callbacks.onremotestream : jQuery.noop;
		callbacks.oncleanup = (typeof callbacks.oncleanup == "function") ? callbacks.oncleanup : jQuery.noop;
		if(!connected) {
			Janus.log("Is the gateway down? (connected=false)");
			callbacks.error("Is the gateway down? (connected=false)");
			return;
		}
		var plugin = callbacks.plugin;
		if(plugin === undefined || plugin === null) {
			Janus.log("Invalid plugin");
			callbacks.error("Invalid plugin");
			return;
		}
		var request = { "janus": "attach", "plugin": plugin, "transaction": randomString(12) };
		$.ajax({
			type: 'POST',
			url: server + "/" + sessionId,
			cache: false,
			contentType: "application/json",
			data: JSON.stringify(request),
			success: function(json) {
				Janus.log("Create handle:");
				Janus.log(json);
				if(json["janus"] !== "success") {
					Janus.log("Ooops: " + json["error"].code + " " + json["error"].reason);	// FIXME
					callbacks.error("Ooops: " + json["error"].code + " " + json["error"].reason);
					return;
				}
				var handleId = json.data["id"];
				Janus.log("Created handle: " + handleId);
				Janus.log(that);
				var pluginHandle =
					{
						session : that,
						plugin : plugin,
						id : handleId,
						webrtcStuff : {
							started : false,
							myStream : null,
							mySdp : null,
							pc : null,
							dtmfSender : null,
							iceDone : false,
							sdpSent : false,
							bitrate : {
								value : null,
								bsnow : null,
								bsbefore : null,
								tsnow : null,
								tsbefore : null,
								timer : null
							}
						},
						getId : function() { return handleId; },
						getPlugin : function() { return plugin; },
						getBitrate : function() { return getBitrate(handleId); },
						send : function(callbacks) { sendMessage(handleId, callbacks); },
						dtmf : function(callbacks) { sendDtmf(handleId, callbacks); },
						consentDialog : callbacks.consentDialog,
						onmessage : callbacks.onmessage,
						createOffer : function(callbacks) { prepareWebrtc(handleId, callbacks); },
						createAnswer : function(callbacks) { prepareWebrtc(handleId, callbacks); },
						handleRemoteJsep : function(callbacks) { prepareWebrtcPeer(handleId, callbacks); },
						onlocalstream : callbacks.onlocalstream,
						onremotestream : callbacks.onremotestream,
						oncleanup : callbacks.oncleanup,
						hangup : function() { cleanupWebrtc(handleId); },
						detach : function(callbacks) { destroyHandle(handleId, callbacks); }
					}
				pluginHandles[handleId] = pluginHandle;
				callbacks.success(pluginHandle);
			},
			error: function(XMLHttpRequest, textStatus, errorThrown) {
				Janus.log(textStatus + ": " + errorThrown);	// FIXME
			},
			dataType: "json"
		});
	}

	// Private method to send a message
	function sendMessage(handleId, callbacks) {
		callbacks = callbacks || {};
		callbacks.success = (typeof callbacks.success == "function") ? callbacks.success : jQuery.noop;
		callbacks.error = (typeof callbacks.error == "function") ? callbacks.error : jQuery.noop;
		if(!connected) {
			Janus.log("Is the gateway down? (connected=false)");
			callbacks.error("Is the gateway down? (connected=false)");
			return;
		}
		var message = callbacks.message;
		var jsep = callbacks.jsep;
		var request = { "janus": "message", "body": message, "transaction": randomString(12) };
		if(jsep !== null && jsep !== undefined)
			request.jsep = jsep;
		Janus.log("Sending message to plugin (handle=" + handleId + "):");
		Janus.log(request);
		$.ajax({
			type: 'POST',
			url: server + "/" + sessionId + "/" + handleId,
			cache: false,
			contentType: "application/json",
			data: JSON.stringify(request),
			success: function(json) {
				Janus.log(json);
				Janus.log("Message sent!");
				if(json["janus"] !== "ack") {
					Janus.log("Ooops: " + json["error"].code + " " + json["error"].reason);	// FIXME
					callbacks.error(json["error"].code + " " + json["error"].reason);
					return;
				}
				callbacks.success();
			},
			error: function(XMLHttpRequest, textStatus, errorThrown) {
				Janus.log(textStatus + ": " + errorThrown);	// FIXME
				callbacks.error(textStatus + ": " + errorThrown);
			},
			dataType: "json"
		});
	}

	// Private method to send a DTMF tone
	function sendDtmf(handleId, callbacks) {
		callbacks = callbacks || {};
		callbacks.success = (typeof callbacks.success == "function") ? callbacks.success : jQuery.noop;
		callbacks.error = (typeof callbacks.error == "function") ? callbacks.error : jQuery.noop;
		var pluginHandle = pluginHandles[handleId];
		var config = pluginHandle.webrtcStuff;
		if(config.dtmfSender === null || config.dtmfSender === undefined) {
			Janus.log("Invalid DTMF configuration");
			callbacks.error("Invalid DTMF configuration");
			return;
		}
		var dtmf = callbacks.dtmf;
		if(dtmf === null || dtmf === undefined) {
			Janus.log("Invalid DTMF parameters");
			callbacks.error("Invalid DTMF parameters");
			return;
		}
		var tones = dtmf.tones;
		if(tones === null || tones === undefined) {
			Janus.log("Invalid DTMF string");
			callbacks.error("Invalid DTMF string");
			return;
		}
		var duration = dtmf.duration;
		if(duration === null || duration === undefined)
			duration = 500;	// We choose 500ms as the default duration for a tone 
		var gap = dtmf.gap;
		if(gap === null || gap === undefined)
			gap = 50;	// We choose 50ms as the default gap between tones
		Janus.log("Sending DTMF string " + tones + " (duration " + duration + "ms, gap " + gap + "ms"); 
		config.dtmfSender.insertDTMF(tones, duration, gap);
	}

	// Private method to destroy a plugin handle
	function destroyHandle(handleId, callbacks, syncRequest) {
		syncRequest = (syncRequest === true);
		Janus.log("Destroying handle " + handleId + "(sync=" + syncRequest + ")");
		callbacks = callbacks || {};
		callbacks.success = (typeof callbacks.success == "function") ? callbacks.success : jQuery.noop;
		callbacks.error = (typeof callbacks.error == "function") ? callbacks.error : jQuery.noop;
		cleanupWebrtc(handleId);
		if(!connected) {
			Janus.log("Is the gateway down? (connected=false)");
			callbacks.error("Is the gateway down? (connected=false)");
			return;
		}
		var request = { "janus": "detach", "transaction": randomString(12) };
		$.ajax({
			type: 'POST',
			url: server + "/" + sessionId + "/" + handleId,
			async: syncRequest,	// Sometimes we need false here, or destroying in onbeforeunload won't work
			cache: false,
			contentType: "application/json",
			data: JSON.stringify(request),
			success: function(json) {
				Janus.log("Destroyed handle:");
				Janus.log(json);
				if(json["janus"] !== "success") {
					Janus.log("Ooops: " + json["error"].code + " " + json["error"].reason);	// FIXME
				}
				var pluginHandle = pluginHandles[handleId];
				delete pluginHandles[handleId];
				callbacks.success();
			},
			error: function(XMLHttpRequest, textStatus, errorThrown) {
				Janus.log(textStatus + ": " + errorThrown);	// FIXME
				// We cleanup anyway
				var pluginHandle = pluginHandles[handleId];
				delete pluginHandles[handleId];
				callbacks.success();
			},
			dataType: "json"
		});
	}
	
	// WebRTC stuff
	function streamsDone(handleId, jsep, media, callbacks, stream) {
		var pluginHandle = pluginHandles[handleId];
		var config = pluginHandle.webrtcStuff;
		if(stream !== null && stream !== undefined)
			Janus.log(stream);
		config.myStream = stream;
		Janus.log("streamsDone:");
		Janus.log(stream);
		var pc_config = {"iceServers": iceServers};
		//~ var pc_constraints = {'mandatory': {'MozDontOfferDataChannel':true}};
		var pc_constraints = {"optional": [{"DtlsSrtpKeyAgreement": true}]};
		config.pc = new RTCPeerConnection(pc_config, pc_constraints);
		Janus.log(config.pc);
		if(config.pc.getStats && webrtcDetectedBrowser == "chrome")	// FIXME
			config.bitrate.value = "0 kbps";
		config.pc.onicecandidate = function(event) {
			if (event.candidate == null) {
				Janus.log("End of candidates.");
				config.iceDone = true;
				if(jsep === null || jsep === undefined)
					setTimeout(function() { createOffer(handleId, media, callbacks); }, 200);
				else
					setTimeout(function() { createAnswer(handleId, media, callbacks); }, 200);
			} else {
				Janus.log("candidates: " + JSON.stringify(event.candidate));
			}
		};
		if(stream !== null && stream !== undefined) {
			Janus.log('Adding local stream');
			config.pc.addStream(stream);
			pluginHandle.onlocalstream(stream);
		}
		config.pc.onaddstream = function(remoteStream) {
			Janus.log("Handling Remote Stream:");
			Janus.log(remoteStream);
			// Start getting the bitrate, if getStats is supported
			if(config.pc.getStats && webrtcDetectedBrowser == "chrome") {	// FIXME
				// http://webrtc.googlecode.com/svn/trunk/samples/js/demos/html/constraints-and-stats.html
				Janus.log("Starting bitrate monitor");
				config.bitrate.timer = setInterval(function() {
					//~ config.pc.getStats(config.pc.getRemoteStreams()[0].getVideoTracks()[0], function(stats) {
					config.pc.getStats(function(stats) {
						var results = stats.result();
						for(var i=0; i<results.length; i++) {
							var res = results[i];
							if(res.type == 'ssrc' && res.stat('googFrameHeightReceived')) {
								config.bitrate.bsnow = res.stat('bytesReceived');
								config.bitrate.tsnow = res.timestamp;
								if(config.bitrate.bsbefore === null || config.bitrate.tsbefore === null) {
									// Skip this round
									config.bitrate.bsbefore = config.bitrate.bsnow;
									config.bitrate.tsbefore = config.bitrate.tsnow;
								} else {
									// Calculate bitrate
									var bitRate = Math.round((config.bitrate.bsnow - config.bitrate.bsbefore) * 8 / (config.bitrate.tsnow - config.bitrate.tsbefore));
									config.bitrate.value = bitRate + ' kbits/sec';
									//~ Janus.log("Estimated bitrate is " + config.bitrate.value);
									config.bitrate.bsbefore = config.bitrate.bsnow;
									config.bitrate.tsbefore = config.bitrate.tsnow;
								}
							}
						}
					});
				}, 1000);
			}
			// Create the DTMF sender too, if possible
			if(config.myStream !== undefined && config.myStream !== null) {
				var tracks = config.myStream.getAudioTracks();
				if(tracks !== null && tracks !== undefined && tracks.length > 0) {
					var local_audio_track = tracks[0];
					config.dtmfSender = config.pc.createDTMFSender(local_audio_track);
					Janus.log("Created DTMF Sender");
					config.dtmfSender.ontonechange = function(tone) { Janus.log("Sent DTMF tone: " + tone.tone); };
				}
			}
			pluginHandle.onremotestream(remoteStream.stream);
		};
		// Create offer/answer now
		if(jsep === null || jsep === undefined) {
			createOffer(handleId, media, callbacks);
		} else {
			config.pc.setRemoteDescription(
					new RTCSessionDescription(jsep),
					function() {
						Janus.log("Remote description accepted!");
					}, callbacks.error);
			createAnswer(handleId, media, callbacks);
		}
	}

	function prepareWebrtc(handleId, callbacks) {
		callbacks = callbacks || {};
		callbacks.success = (typeof callbacks.success == "function") ? callbacks.success : jQuery.noop;
		callbacks.error = (typeof callbacks.error == "function") ? callbacks.error : webrtcError;
		var jsep = callbacks.jsep;
		var media = callbacks.media;
		var pluginHandle = pluginHandles[handleId];
		var config = pluginHandle.webrtcStuff;
		if(isAudioSendEnabled(media) || isVideoSendEnabled(media)) {
			var constraints = { mandatory: {}, optional: []};
			pluginHandle.consentDialog(true);
			var videoSupport = isVideoSendEnabled(media);
			if(videoSupport === true && media != undefined && media != null) {
				if(media.video === 'lowres') {
					// Add a video constraint (320x240)
					if(!navigator.mozGetUserMedia) {
						videoSupport = {"mandatory": {"maxHeight": "240", "maxWidth": "320"}, "optional": []};
						Janus.log("Adding media constraint (low-res video)");
						Janus.log(videoSupport);
					} else {
						Janus.log("Firefox doesn't support media constraints at the moment, ignoring low-res video");
					}
				} else if(media.video === 'hires') {
					// Add a video constraint (1280x720)
					if(!navigator.mozGetUserMedia) {
						videoSupport = {"mandatory": {"minHeight": "720", "minWidth": "1280"}, "optional": []};
						Janus.log("Adding media constraint (hi-res video)");
						Janus.log(videoSupport);
					} else {
						Janus.log("Firefox doesn't support media constraints at the moment, ignoring hi-res video");
					}
				}
			}
			getUserMedia(
				{audio:isAudioSendEnabled(media), video:videoSupport},
				function(stream) { pluginHandle.consentDialog(false); streamsDone(handleId, jsep, media, callbacks, stream); },
				function(error) { pluginHandle.consentDialog(false); callbacks.error(error); });
		} else {
			// No need to do a getUserMedia, create offer/answer right away
			streamsDone(handleId, jsep, media, callbacks);
		}
	}

	function prepareWebrtcPeer(handleId, callbacks) {
		callbacks = callbacks || {};
		callbacks.success = (typeof callbacks.success == "function") ? callbacks.success : jQuery.noop;
		callbacks.error = (typeof callbacks.error == "function") ? callbacks.error : webrtcError;
		var jsep = callbacks.jsep;
		var pluginHandle = pluginHandles[handleId];
		var config = pluginHandle.webrtcStuff;
		if(jsep !== undefined && jsep !== null) {
			if(config.pc === null) {
				Janus.log("Wait, no PeerConnection?? if this is an answer, use createAnswer and not handleRemoteJsep");
				callbacks.error("No PeerConnection: if this is an answer, use createAnswer and not handleRemoteJsep");
				return;
			}
			config.pc.setRemoteDescription(
					new RTCSessionDescription(jsep),
					function() {
						Janus.log("Remote description accepted!");
						callbacks.success();
					}, callbacks.error);
		} else {
			callbacks.error("Invalid JSEP");
		}
	}

	function createOffer(handleId, media, callbacks) {
		callbacks = callbacks || {};
		callbacks.success = (typeof callbacks.success == "function") ? callbacks.success : jQuery.noop;
		callbacks.error = (typeof callbacks.error == "function") ? callbacks.error : jQuery.noop;
		var pluginHandle = pluginHandles[handleId];
		var config = pluginHandle.webrtcStuff;
		Janus.log("Creating offer (iceDone=" + config.iceDone + ")");
		var mediaConstraints = {'mandatory': {
									'OfferToReceiveAudio':isAudioRecvEnabled(media), 
									'OfferToReceiveVideo':isVideoRecvEnabled(media) }};
		Janus.log(mediaConstraints);
		config.pc.createOffer(
			function(offer) {
				Janus.log(offer);
				if(config.mySdp == offer.sdp) {
					Janus.log("Just got the same offer again?");
				} else {
					Janus.log("Setting local description");
					config.mySdp = offer.sdp;
					config.pc.setLocalDescription(offer);
				}
				if(!navigator.mozGetUserMedia && !config.iceDone) {
					// Don't do anything until we have all candidates (but only for Chrome)
					Janus.log("Waiting for all candidates...");
					return;
				}
				if(config.sdpSent) {
					Janus.log("Offer already sent, not sending it again");
					return;
				}
				Janus.log("Offer ready");
				Janus.log(callbacks);
				config.sdpSent = true;
				callbacks.success(offer);
			}, callbacks.error, mediaConstraints);
	}
	
	function createAnswer(handleId, media, callbacks) {
		callbacks = callbacks || {};
		callbacks.success = (typeof callbacks.success == "function") ? callbacks.success : jQuery.noop;
		callbacks.error = (typeof callbacks.error == "function") ? callbacks.error : jQuery.noop;
		var pluginHandle = pluginHandles[handleId];
		var config = pluginHandle.webrtcStuff;
		Janus.log("Creating answer (iceDone=" + config.iceDone + ")");
		var mediaConstraints = {'mandatory': {
									'OfferToReceiveAudio':isAudioRecvEnabled(media), 
									'OfferToReceiveVideo':isVideoRecvEnabled(media) }};
		Janus.log(mediaConstraints);
		config.pc.createAnswer(
			function(answer) {
				Janus.log(answer);
				if(config.mySdp == answer.sdp) {
					Janus.log("Just got the same answer again?");
				} else {
					config.mySdp = answer.sdp;
					config.pc.setLocalDescription(answer);
				}
				if(!navigator.mozGetUserMedia && !config.iceDone) {
					// Don't do anything until we have all candidates (but only for Chrome)
					Janus.log("Waiting for all candidates...");
					return;
				}
				if(config.sdpSent) {	// FIXME badly
					Janus.log("Answer already sent, not sending it again");
					return;
				}
				config.sdpSent = true;
				callbacks.success(answer);
			}, callbacks.error, mediaConstraints);
	}

	function getBitrate(handleId) {
		var pluginHandle = pluginHandles[handleId];
		var config = pluginHandle.webrtcStuff;
		//~ Janus.log(pluginHandle);
		//~ Janus.log(config);
		//~ Janus.log(config.bitrate);
		if(config.bitrate.value === undefined || config.bitrate.value === null)
			return "Feature unsupported by browser";
		return config.bitrate.value;
	}
	
	function webrtcError(error) {
		Janus.log("WebRTC error:");
		Janus.log(error);
	}

	function cleanupWebrtc(handleId) {
		Janus.log("Cleaning WebRTC stuff");
		var pluginHandle = pluginHandles[handleId];
		var config = pluginHandle.webrtcStuff;
		// Cleanup
		if(config.bitrate.timer)
			clearInterval(config.bitrate.timer);
		config.bitrate.timer = null;
		config.bitrate.bsnow = null;
		config.bitrate.bsbefore = null;
		config.bitrate.tsnow = null;
		config.bitrate.tsbefore = null;
		config.bitrate.value = null;
		if(config.myStream !== null && config.myStream !== undefined) {
			Janus.log("Stopping local stream");
			config.myStream.stop();
		}
		config.myStream = null;
		// Close PeerConnection
		try {
			config.pc.close();
		} catch(e) {
			// Do nothing
		}
		config.pc = null;
		config.mySdp = null;
		config.iceDone = false;
		config.sdpSent = false;
		pluginHandle.oncleanup();
	}

	// Helper methods to parse a media object
	function isAudioSendEnabled(media) {
		Janus.log("isAudioSendEnabled:");
		Janus.log(media);
		if(media === undefined || media === null)
			return true;	// Default
		if(media.audio === false)
			return false;	// Generic audio has precedence
		if(media.audioSend === undefined || media.audioSend === null)
			return true;	// Default
		return (media.audioSend === true);
	}

	function isAudioRecvEnabled(media) {
		Janus.log("isAudioRecvEnabled:");
		Janus.log(media);
		if(media === undefined || media === null)
			return true;	// Default
		if(media.audio === false)
			return false;	// Generic audio has precedence
		if(media.audioRecv === undefined || media.audioRecv === null)
			return true;	// Default
		return (media.audioRecv === true);
	}

	function isVideoSendEnabled(media) {
		Janus.log("isVideoSendEnabled:");
		Janus.log(media);
		if(media === undefined || media === null)
			return true;	// Default
		if(media.video === false)
			return false;	// Generic video has precedence
		if(media.videoSend === undefined || media.videoSend === null)
			return true;	// Default
		return (media.videoSend === true);
	}

	function isVideoRecvEnabled(media) {
		Janus.log("isVideoRecvEnabled:");
		Janus.log(media);
		if(media === undefined || media === null)
			return true;	// Default
		if(media.video === false)
			return false;	// Generic video has precedence
		if(media.videoRecv === undefined || media.videoRecv === null)
			return true;	// Default
		return (media.videoRecv === true);
	}
};
