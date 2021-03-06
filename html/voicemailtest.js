var server = "http://" + window.location.hostname + ":8088/janus";

var janus = null;
var vmailtest = null;
var started = false;
var spinner = null;

var myusername = null;
var myid = null;
var audioenabled = false;


$(document).ready(function() {
	// Initialize the library (console debug enabled)
	Janus.init({debug: true, callback: function() {
		// Use a button to start the demo
		$('#start').click(function() {
			if(started)
				return;
			started = true;
			$(this).attr('disabled', true).unbind('click');
			// Make sure the browser supports WebRTC
			if(!Janus.isWebrtcSupported()) {
				bootbox.alert("No WebRTC support... ");
				return;
			}
			// Create session
			janus = new Janus(
				{
					server: server,
					success: function() {
						// Attach to Voice Mail test plugin
						janus.attach(
							{
								plugin: "janus.plugin.voicemail",
								success: function(pluginHandle) {
									vmailtest = pluginHandle;
									console.log("Plugin attached! (" + vmailtest.getPlugin() + ", id=" + vmailtest.getId() + ")");
									$('#voicemail').removeClass('hide').show();
									$('#start').removeAttr('disabled').html("Stop")
										.click(function() {
											$(this).attr('disabled', true);
											janus.destroy();
										});
									$('#record').removeAttr('disabled').html("Record")
										.click(function() {
											$(this).attr('disabled', true);
											startRecording();
										});
								},
								error: function(error) {
									console.log("  -- Error attaching plugin... " + error);
									bootbox.alert("Error attaching plugin... " + error);
								},
								consentDialog: function(on) {
									console.log("Consent dialog should be " + (on ? "on" : "off") + " now");
									if(on) {
										// Darken screen and show hint
										$.blockUI({ 
											message: '<div><img src="up_arrow.png"/></div>',
											css: {
												border: 'none',
												padding: '15px',
												backgroundColor: 'transparent',
												color: '#aaa',
												top: '10px',
												left: (navigator.mozGetUserMedia ? '-100px' : '400px')
											} });
									} else {
										// Restore screen
										$.unblockUI();
									}
								},
								onmessage: function(msg, jsep) {
									console.log(" ::: Got a message :::");
									console.log(JSON.stringify(msg));
									var event = msg["voicemail"];
									console.log("Event: " + event);
									if(event != undefined && event != null) {
										if(event === "event") {
											if(msg["status"] !== undefined && msg["status"] !== null) {
												var status = msg["status"];
												if(status === 'starting') {
													$('#record')
														.removeClass("btn-danger").addClass("btn-default")
														.text("Starting, please wait...");
												} else if(status === 'started') {
													$('#record')
														.removeClass("btn-default").addClass("btn-info")
														.text("Started");
												} else if(status === 'done') {
													$('#record')
														.removeClass("btn-info").addClass("btn-success")
														.text("Done!");
													$('#download').attr('href', msg["recording"]);
													$('#listen').click(function() {
														$('#rec').remove();
														$('#done').parent().append(
															//~ '<audio id="rec" style="width:100%;height:100%;" autoplay>' +
																//~ '<source src="' + msg["recording"] + '" type="audio/ogg"></source>' +
															//~ '</audio>'
															'<audio id="rec" style="width:100%;height:100%;" autoplay controls ' +
																'src="' + msg["recording"] + '">' +
															'</audio>'
														);
														return false;
													});
													$('#done').removeClass('hide').show();
													vmailtest.hangup();
												}
											} else if(msg["error"] !== undefined && msg["error"] !== null) {
												bootbox.alert(msg["error"], function() {
													window.location.reload();
												});
											}
										}
									}
									if(jsep !== undefined && jsep !== null) {
										console.log("Handling SDP as well...");
										console.log(jsep.sdp);
										vmailtest.handleRemoteJsep({jsep: jsep});
									}
								},
								onlocalstream: function(stream) {
									// We're not going to attach the local audio stream
								},
								onremotestream: function(stream) {
									// We're not going to receive anything from the plugin
								},
								oncleanup: function() {
									console.log(" ::: Got a cleanup notification :::");
								}
							});
					},
					error: function(error) {
						console.log(error);
						bootbox.alert(error, function() {
							window.location.reload();
						});
					},
					destroyed: function() {
						window.location.reload();
					}
				});
		});
	}});
});

function startRecording() {
	// Negotiate WebRTC now
	vmailtest.createOffer(
		{
			media: { audioRecv: false, video: false},	// We're going to only send, and not receive, audio
			success: function(jsep) {
				console.log("Got SDP!");
				console.log(jsep.sdp);
				var publish = { "request": "record" };
				vmailtest.send({"message": publish, "jsep": jsep});
			},
			error: function(error) {
				console.log("WebRTC error:");
				console.log(error);
				bootbox.alert("WebRTC error... " + error);
			}
		});
}
