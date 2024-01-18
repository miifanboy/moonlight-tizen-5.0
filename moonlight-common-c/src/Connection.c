#include "Limelight-internal.h"
#include "Platform.h"

static int stage = STAGE_NONE;
static ConnListenerConnectionTerminated originalTerminationCallback;
static int alreadyTerminated;
static PLT_THREAD terminationCallbackThread;
static int terminationCallbackErrorCode;

// Common globals
char* RemoteAddrString;
struct sockaddr_storage RemoteAddr;
SOCKADDR_LEN RemoteAddrLen;
int AppVersionQuad[4];
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
DECODER_RENDERER_CALLBACKS VideoCallbacks;
AUDIO_RENDERER_CALLBACKS AudioCallbacks;
int NegotiatedVideoFormat;
volatile int ConnectionInterrupted;
int HighQualitySurroundSupported;
int HighQualitySurroundEnabled;
int OriginalVideoBitrate;
int AudioPacketDuration;

// Connection stages
static const char* stageNames[STAGE_MAX] = {
    "none",
    "platform initialization",
    "name resolution",
    "RTSP handshake",
    "control stream initialization",
    "video stream initialization",
    "audio stream initialization",
    "input stream initialization",
    "control stream establishment",
    "video stream establishment",
    "audio stream establishment",
    "input stream establishment"
};

// Get the name of the current stage based on its number
const char* LiGetStageName(int stage) {
    return stageNames[stage];
}

// Interrupt a pending connection attempt. This interruption happens asynchronously
// so it is not safe to start another connection before LiStartConnection() returns.
void LiInterruptConnection(void) {
    // Signal anyone waiting on the global interrupted flag
    ConnectionInterrupted = 1;
}

// Stop the connection by undoing the step at the current stage and those before it
void LiStopConnection(void) {
    // Disable termination callbacks now
    alreadyTerminated = 1;

    // Set the interrupted flag
    LiInterruptConnection();

    if (stage == STAGE_INPUT_STREAM_START) {
        Limelog("Stopping input stream...");
        stopInputStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_AUDIO_STREAM_START) {
        Limelog("Stopping audio stream...");
        stopAudioStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_VIDEO_STREAM_START) {
        Limelog("Stopping video stream...");
        stopVideoStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_CONTROL_STREAM_START) {
        Limelog("Stopping control stream...");
        stopControlStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_INPUT_STREAM_INIT) {
        Limelog("Cleaning up input stream...");
        destroyInputStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_AUDIO_STREAM_INIT) {
        Limelog("Cleaning up audio stream...");
        destroyAudioStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_VIDEO_STREAM_INIT) {
        Limelog("Cleaning up video stream...");
        destroyVideoStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_CONTROL_STREAM_INIT) {
        Limelog("Cleaning up control stream...");
        destroyControlStream();
        stage--;
        Limelog("done\n");
    }
    if (stage == STAGE_RTSP_HANDSHAKE) {
        // Nothing to do
        stage--;
    }
    if (stage == STAGE_NAME_RESOLUTION) {
        // Nothing to do
        stage--;
    }
    if (stage == STAGE_PLATFORM_INIT) {
        Limelog("Cleaning up platform...");
        cleanupPlatform();
        stage--;
        Limelog("done\n");
    }
    LC_ASSERT(stage == STAGE_NONE);
    
    if (RemoteAddrString != NULL) {
        free(RemoteAddrString);
        RemoteAddrString = NULL;
    }
}

static void terminationCallbackThreadFunc(void* context)
{
    // Invoke the client's termination callback
    originalTerminationCallback(terminationCallbackErrorCode);
}

// This shim callback runs the client's connectionTerminated() callback on a
// separate thread. This is neccessary because other internal threads directly
// invoke this callback. That can result in a deadlock if the client
// calls LiStopConnection() in the callback when the cleanup code
// attempts to join the thread that the termination callback (and LiStopConnection)
// is running on.
static void ClInternalConnectionTerminated(int errorCode)
{
    int err;

    // Avoid recursion and issuing multiple callbacks
    if (alreadyTerminated || ConnectionInterrupted) {
        return;
    }

    terminationCallbackErrorCode = errorCode;
    alreadyTerminated = 1;

    // Invoke the termination callback on a separate thread
    err = PltCreateThread("AsyncTerm", terminationCallbackThreadFunc, NULL, &terminationCallbackThread);
    if (err != 0) {
        // Nothing we can safely do here, so we'll just assert on debug builds
        Limelog("Failed to create termination thread: %d\n", err);
        LC_ASSERT(err == 0);
    }

    // Close the thread handle since we can never wait on it
    PltCloseThread(&terminationCallbackThread);
}

// Starts the connection to the streaming machine
int LiStartConnection(PSERVER_INFORMATION serverInfo, PSTREAM_CONFIGURATION streamConfig, PCONNECTION_LISTENER_CALLBACKS clCallbacks,
    PDECODER_RENDERER_CALLBACKS drCallbacks, PAUDIO_RENDERER_CALLBACKS arCallbacks, void* renderContext, int drFlags,
    void* audioContext, int arFlags) {
    int err;

    NegotiatedVideoFormat = 0;
    memcpy(&StreamConfig, streamConfig, sizeof(StreamConfig));
    OriginalVideoBitrate = streamConfig->bitrate;
    RemoteAddrString = strdup(serverInfo->address);
    
    // FEC only works in 16 byte chunks, so we must round down
    // the given packet size to the nearest multiple of 16.
    StreamConfig.packetSize -= StreamConfig.packetSize % 16;

    if (StreamConfig.packetSize == 0) {
        Limelog("Invalid packet size specified\n");
        err = -1;
        goto Cleanup;
    }
    
    // Extract the appversion from the supplied string
    if (extractVersionQuadFromString(serverInfo->serverInfoAppVersion,
                                     AppVersionQuad) < 0) {
        Limelog("Invalid appversion string: %s\n", serverInfo->serverInfoAppVersion);
        err = -1;
        goto Cleanup;
    }

    // Replace missing callbacks with placeholders
    fixupMissingCallbacks(&drCallbacks, &arCallbacks, &clCallbacks);
    memcpy(&VideoCallbacks, drCallbacks, sizeof(VideoCallbacks));
    memcpy(&AudioCallbacks, arCallbacks, sizeof(AudioCallbacks));

    // Hook the termination callback so we can avoid issuing a termination callback
    // after LiStopConnection() is called
    originalTerminationCallback = clCallbacks->connectionTerminated;
    memcpy(&ListenerCallbacks, clCallbacks, sizeof(ListenerCallbacks));
    ListenerCallbacks.connectionTerminated = ClInternalConnectionTerminated;

    alreadyTerminated = 0;
    ConnectionInterrupted = 0;

    Limelog("Initializing platform...");
    ListenerCallbacks.stageStarting(STAGE_PLATFORM_INIT);
    err = initializePlatform();
    if (err != 0) {
        Limelog("failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_PLATFORM_INIT, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_PLATFORM_INIT);
    ListenerCallbacks.stageComplete(STAGE_PLATFORM_INIT);
    Limelog("done\n");

    Limelog("Resolving host name...");
    ListenerCallbacks.stageStarting(STAGE_NAME_RESOLUTION);
    err = resolveHostName(serverInfo->address, AF_UNSPEC, 47984, &RemoteAddr, &RemoteAddrLen);
    if (err != 0) {
        Limelog("failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_NAME_RESOLUTION, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_NAME_RESOLUTION);
    ListenerCallbacks.stageComplete(STAGE_NAME_RESOLUTION);
    Limelog("done\n");

    // If STREAM_CFG_AUTO was requested, determine the streamingRemotely value
    // now that we have resolved the target address and impose the video packet
    // size cap if required.
    if (StreamConfig.streamingRemotely == STREAM_CFG_AUTO) {
        if (isPrivateNetworkAddress(&RemoteAddr)) {
            StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
        }
        else {
            StreamConfig.streamingRemotely = STREAM_CFG_REMOTE;

            if (StreamConfig.packetSize > 1024) {
                // Cap packet size at 1024 for remote streaming to avoid
                // MTU problems and fragmentation.
                Limelog("Packet size capped at 1KB for remote streaming\n");
                StreamConfig.packetSize = 1024;
            }
        }
    }

    Limelog("Starting RTSP handshake...");
    ListenerCallbacks.stageStarting(STAGE_RTSP_HANDSHAKE);
    err = performRtspHandshake();
    if (err != 0) {
        Limelog("failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_RTSP_HANDSHAKE, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_RTSP_HANDSHAKE);
    ListenerCallbacks.stageComplete(STAGE_RTSP_HANDSHAKE);
    Limelog("done\n");

    Limelog("Initializing control stream...");
    ListenerCallbacks.stageStarting(STAGE_CONTROL_STREAM_INIT);
    err = initializeControlStream();
    if (err != 0) {
        Limelog("failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_CONTROL_STREAM_INIT, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_CONTROL_STREAM_INIT);
    ListenerCallbacks.stageComplete(STAGE_CONTROL_STREAM_INIT);
    Limelog("done\n");

    Limelog("Initializing video stream...");
    ListenerCallbacks.stageStarting(STAGE_VIDEO_STREAM_INIT);
    initializeVideoStream();
    stage++;
    LC_ASSERT(stage == STAGE_VIDEO_STREAM_INIT);
    ListenerCallbacks.stageComplete(STAGE_VIDEO_STREAM_INIT);
    Limelog("done\n");

    Limelog("Initializing audio stream...");
    ListenerCallbacks.stageStarting(STAGE_AUDIO_STREAM_INIT);
    initializeAudioStream();
    stage++;
    LC_ASSERT(stage == STAGE_AUDIO_STREAM_INIT);
    ListenerCallbacks.stageComplete(STAGE_AUDIO_STREAM_INIT);
    Limelog("done\n");

    Limelog("Initializing input stream...");
    ListenerCallbacks.stageStarting(STAGE_INPUT_STREAM_INIT);
    initializeInputStream();
    stage++;
    LC_ASSERT(stage == STAGE_INPUT_STREAM_INIT);
    ListenerCallbacks.stageComplete(STAGE_INPUT_STREAM_INIT);
    Limelog("done\n");

    Limelog("Starting control stream...");
    ListenerCallbacks.stageStarting(STAGE_CONTROL_STREAM_START);
    err = startControlStream();
    if (err != 0) {
        Limelog("failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_CONTROL_STREAM_START, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_CONTROL_STREAM_START);
    ListenerCallbacks.stageComplete(STAGE_CONTROL_STREAM_START);
    Limelog("done\n");

    Limelog("Starting video stream...");
    ListenerCallbacks.stageStarting(STAGE_VIDEO_STREAM_START);
    err = startVideoStream(renderContext, drFlags);
    if (err != 0) {
        Limelog("Video stream start failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_VIDEO_STREAM_START, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_VIDEO_STREAM_START);
    ListenerCallbacks.stageComplete(STAGE_VIDEO_STREAM_START);
    Limelog("done\n");

    Limelog("Starting audio stream...");
    ListenerCallbacks.stageStarting(STAGE_AUDIO_STREAM_START);
    err = startAudioStream(audioContext, arFlags);
    if (err != 0) {
        Limelog("Audio stream start failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_AUDIO_STREAM_START, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_AUDIO_STREAM_START);
    ListenerCallbacks.stageComplete(STAGE_AUDIO_STREAM_START);
    Limelog("done\n");

    Limelog("Starting input stream...");
    ListenerCallbacks.stageStarting(STAGE_INPUT_STREAM_START);
    err = startInputStream();
    if (err != 0) {
        Limelog("Input stream start failed: %d\n", err);
        ListenerCallbacks.stageFailed(STAGE_INPUT_STREAM_START, err);
        goto Cleanup;
    }
    stage++;
    LC_ASSERT(stage == STAGE_INPUT_STREAM_START);
    ListenerCallbacks.stageComplete(STAGE_INPUT_STREAM_START);
    Limelog("done\n");
    
    // Wiggle the mouse a bit to wake the display up
    LiSendMouseMoveEvent(1, 1);
    PltSleepMs(10);
    LiSendMouseMoveEvent(-1, -1);
    PltSleepMs(10);

    ListenerCallbacks.connectionStarted();

Cleanup:
    if (err != 0) {
        // Undo any work we've done here before failing
        LiStopConnection();
    }
    return err;
}
