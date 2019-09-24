/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>
#include <ctime>
#include <set>

#include <boost/algorithm/string/split.hpp>
#include <glog/logging.h>

// Include dummy folly header to avoid unambiguous references to pid_t (OVR_Types.h and
// folly/portability/Unistd.h)
#include <folly/FileUtil.h>
#include <folly/dynamic.h>
#include <folly/json.h>

#include "source/render/RigScene.h"
#include "source/render/Soundtrack.h"
#include "source/render/VideoFile.h"
#include "source/viewer/MenuScreen.h"
#include "source/util/SystemUtil.h"

using namespace fb360_dep;


const std::string kUsageMessage = R"(
   Plays a video sequence on a Rift HMD, taking as input the fused binary file, catalog, and rig
   file generated by ConvertToBinary.

   Keyboard navigation:
   - W/UP, A, S/DOWN, and D translate forward, left, back, and right, respectively
   - LEFT and RIGHT rotate left and right, respectively
   - C recenters the view

   Keyboard controls:
   - SPACE pauses and plays the video
   - ESC/ctrl-Q closes the app
   - H toggles headbox fade-out
   - B toggles background rendering (experimental)

   Example:
     ./RiftViewer.exe \
     --rig=$ConvertToBinaryOutputDirectory/rig_fused.json \
     --catalog=$ConvertToBinaryOutputDirectory/fused.json \
     --strip_files=$ConvertToBinaryOutputDirectory/fused_0.bin
 )";

DEFINE_string(audio, "", "optional .tbe audio file");
DEFINE_string(background_catalog, "", "optional path to catalog for background (experimental)");
DEFINE_string(background_file, "", "optional single strip file for background (experimental)");
DEFINE_string(catalog, "", "path to catalog file (required)");
DEFINE_int32(fps, 30, "video framerate");
DEFINE_string(rig, "", "path to rig.json (required)");
DEFINE_string(strip_files, "", "comma-separated list of strip files (required)");

struct OculusTextureBuffer {
  ovrSession Session;
  ovrTextureSwapChain ColorTextureChain;
  ovrTextureSwapChain DepthTextureChain;
  GLuint fboId;
  Sizei texSize;

  OculusTextureBuffer(ovrSession session, Sizei size, int sampleCount)
      : Session(session),
        ColorTextureChain(nullptr),
        DepthTextureChain(nullptr),
        fboId(0),
        texSize(0, 0) {
    assert(sampleCount <= 1); // The code doesn't currently handle MSAA textures.

    texSize = size;

    // This texture isn't necessarily going to be a rendertarget, but it usually is.
    assert(session); // No HMD? A little odd.

    ovrTextureSwapChainDesc desc = {};
    desc.Type = ovrTexture_2D;
    desc.ArraySize = 1;
    desc.Width = size.w;
    desc.Height = size.h;
    desc.MipLevels = 1;
    desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
    desc.SampleCount = sampleCount;
    desc.StaticImage = ovrFalse;

    {
      ovrResult result = ovr_CreateTextureSwapChainGL(Session, &desc, &ColorTextureChain);

      int length = 0;
      ovr_GetTextureSwapChainLength(session, ColorTextureChain, &length);

      if (OVR_SUCCESS(result)) {
        for (int i = 0; i < length; ++i) {
          GLuint chainTexId;
          ovr_GetTextureSwapChainBufferGL(Session, ColorTextureChain, i, &chainTexId);
          glBindTexture(GL_TEXTURE_2D, chainTexId);

          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
      }
    }

    desc.Format = OVR_FORMAT_D32_FLOAT;

    {
      ovrResult result = ovr_CreateTextureSwapChainGL(Session, &desc, &DepthTextureChain);

      int length = 0;
      ovr_GetTextureSwapChainLength(session, DepthTextureChain, &length);

      if (OVR_SUCCESS(result)) {
        for (int i = 0; i < length; ++i) {
          GLuint chainTexId;
          ovr_GetTextureSwapChainBufferGL(Session, DepthTextureChain, i, &chainTexId);
          glBindTexture(GL_TEXTURE_2D, chainTexId);

          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
      }
    }

    glGenFramebuffers(1, &fboId);
  }

  ~OculusTextureBuffer() {
    if (ColorTextureChain) {
      ovr_DestroyTextureSwapChain(Session, ColorTextureChain);
      ColorTextureChain = nullptr;
    }
    if (DepthTextureChain) {
      ovr_DestroyTextureSwapChain(Session, DepthTextureChain);
      DepthTextureChain = nullptr;
    }
    if (fboId) {
      glDeleteFramebuffers(1, &fboId);
      fboId = 0;
    }
  }

  Sizei GetSize() const {
    return texSize;
  }

  void SetAndClearRenderSurface() {
    GLuint curColorTexId;
    GLuint curDepthTexId;
    {
      int curIndex;
      ovr_GetTextureSwapChainCurrentIndex(Session, ColorTextureChain, &curIndex);
      ovr_GetTextureSwapChainBufferGL(Session, ColorTextureChain, curIndex, &curColorTexId);
    }
    {
      int curIndex;
      ovr_GetTextureSwapChainCurrentIndex(Session, DepthTextureChain, &curIndex);
      ovr_GetTextureSwapChainBufferGL(Session, DepthTextureChain, curIndex, &curDepthTexId);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fboId);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, curColorTexId, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, curDepthTexId, 0);

    glViewport(0, 0, texSize.w, texSize.h);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_FRAMEBUFFER_SRGB);
  }

  void UnsetRenderSurface() {
    glBindFramebuffer(GL_FRAMEBUFFER, fboId);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
  }

  void Commit() {
    ovr_CommitTextureSwapChain(Session, ColorTextureChain);
    ovr_CommitTextureSwapChain(Session, DepthTextureChain);
  }
};

static void verifyInputs() {
  CHECK_NE(FLAGS_rig, "");
  CHECK_NE(FLAGS_catalog, "");
  CHECK_NE(FLAGS_strip_files, "");
  if (!FLAGS_background_catalog.empty()) {
    CHECK_NE(FLAGS_background_file, "");
  }
}

typedef std::chrono::time_point<std::chrono::high_resolution_clock> Time;

static Time startTime;

static Time getCurrentTime() {
  return std::chrono::high_resolution_clock::now();
}

static float getVideoTimeMs(const VideoFile& videoFile) {
  return videoFile.getFront() * (1000.f / FLAGS_fps);
}

static std::set<char> depressedKeys;

static std::vector<char> getAndUpdateActiveKeys() {
  static std::set<char> keysOfInterest =
    {VK_LEFT, VK_RIGHT, 'W', VK_UP, 'S', VK_DOWN, 'D', 'A', 'C', 'H', 'M', 'B', ' '};
  static std::set<char> keysActiveOnlyOnKeyPress = {'C', 'H', 'M', 'B', ' '};

  std::vector<char> activeKeys;
  for (char c : keysOfInterest) {
    if (Platform.Key[c]) {
      if (depressedKeys.find(c) == depressedKeys.end() ||
          keysActiveOnlyOnKeyPress.find(c) == keysActiveOnlyOnKeyPress.end()) {
        depressedKeys.insert(c);
        activeKeys.push_back(c);
      }
    } else {
      depressedKeys.erase(c);
    }
  }
  return activeKeys;
}

// return true to retry later (e.g. after display lost)
static bool MainLoop(bool retryCreate) {
  OculusTextureBuffer* eyeRenderTexture[2] = {nullptr, nullptr};
  ovrMirrorTexture mirrorTexture = nullptr;
  GLuint mirrorFBO = 0;
  long long frameIndex = 0;

  ovrSession session;
  ovrGraphicsLuid luid;
  ovrResult result = ovr_Create(&session, &luid);
  if (!OVR_SUCCESS(result))
    return retryCreate;

  ovrHmdDesc hmdDesc = ovr_GetHmdDesc(session);

  // Setup Window and Graphics
  // Note: the mirror window can be any size, for this sample we use 1/2 the HMD resolution
  ovrSizei windowSize = {hmdDesc.Resolution.w / 2, hmdDesc.Resolution.h / 2};
  if (!Platform.InitDevice(windowSize.w, windowSize.h, reinterpret_cast<LUID*>(&luid)))
    goto Done;

  // Make eye render buffers
  for (int eye = 0; eye < 2; ++eye) {
    ovrSizei idealTextureSize =
        ovr_GetFovTextureSize(session, ovrEyeType(eye), hmdDesc.DefaultEyeFov[eye], 1);
    eyeRenderTexture[eye] = new OculusTextureBuffer(session, idealTextureSize, 1);

    if (!eyeRenderTexture[eye]->ColorTextureChain || !eyeRenderTexture[eye]->DepthTextureChain) {
      if (retryCreate)
        goto Done;
      VALIDATE(false, "Failed to create texture.");
    }
  }

  ovrMirrorTextureDesc desc;
  memset(&desc, 0, sizeof(desc));
  desc.Width = windowSize.w;
  desc.Height = windowSize.h;
  desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;

  // Create mirror texture and an FBO used to copy mirror texture to back buffer
  result = ovr_CreateMirrorTextureGL(session, &desc, &mirrorTexture);
  if (!OVR_SUCCESS(result)) {
    if (retryCreate)
      goto Done;
    VALIDATE(false, "Failed to create mirror texture.");
  }

  // Configure the mirror read buffer
  GLuint texId;
  ovr_GetMirrorTextureBufferGL(session, mirrorTexture, &texId);

  glGenFramebuffers(1, &mirrorFBO);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, mirrorFBO);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texId, 0);
  glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

  // Turn off vsync to let the compositor do its magic
  wglSwapIntervalEXT(0);

  // FloorLevel will give tracking poses where the floor height is 0
  ovr_SetTrackingOriginType(session, ovrTrackingOrigin_EyeLevel);

  {
    verifyInputs();

    // create the scene
    RigScene scene(FLAGS_rig);

    // load background geometry
    if (!FLAGS_background_catalog.empty()) {
      LOG(INFO) << "Loading background geometry";
      VideoFile videoFile(FLAGS_background_catalog, {FLAGS_background_file});
      CHECK(videoFile.frames.size() == 1);
      videoFile.readBegin(scene);
      scene.backgroundSubframes = videoFile.readEnd(scene);
    }

    // create the video and begin loading the first frame
    std::vector<std::string> v;
    boost::algorithm::split(v, FLAGS_strip_files, [](char c) { return c == ','; });
    VideoFile videoFile(FLAGS_catalog, v);
    if (videoFile.frames.size() == 1) {
      // special case a single-frame video
      videoFile.readBegin(scene);
      scene.subframes = videoFile.readEnd(scene);
    } else {
      for (int readahead = 0; readahead < 3; ++readahead) {
        videoFile.readBegin(scene);
      }
    }

    // create soundtrack and load it, if requested
    Soundtrack soundtrack;
    if (!FLAGS_audio.empty()) {
      soundtrack.load(FLAGS_audio);
    }

    static bool pause = true;
    static bool started = false;
    static int const kHeadboxFade = 2;
    static int fade = kHeadboxFade;

    MenuScreen menu;
    menu.exitMenuCallback = [&] {
      ovr_RecenterTrackingOrigin(session);
      pause = false;
    };

    // Main loop
    while (Platform.HandleMessages()) {
      ovrSessionStatus sessionStatus;
      ovr_GetSessionStatus(session, &sessionStatus);
      if (sessionStatus.ShouldQuit) {
        // Because the application is requested to quit, should not request retry
        retryCreate = false;
        break;
      }

      if (sessionStatus.IsVisible) {
        static float Yaw(3.141592f);
        static Vector3f Pos2(0.0f, 0.0f, 0.0f);

        std::vector<char> activeKeys = getAndUpdateActiveKeys();
        for (char key : activeKeys) {
          switch (key) {
            // Manual control of position and orientation
            case VK_LEFT:
              Yaw += 0.02f;
              break;
            case VK_RIGHT:
              Yaw -= 0.02f;
              break;
            case 'W':
            case VK_UP:
              Pos2 += Matrix4f::RotationY(Yaw).Transform(Vector3f(0, 0, -0.05f));
              break;
            case 'S':
            case VK_DOWN:
              Pos2 += Matrix4f::RotationY(Yaw).Transform(Vector3f(0, 0, +0.05f));
              break;
            case 'D':
              Pos2 += Matrix4f::RotationY(Yaw).Transform(Vector3f(+0.05f, 0, 0));
              break;
            case 'A':
              Pos2 += Matrix4f::RotationY(Yaw).Transform(Vector3f(-0.05f, 0, 0));
              break;

            // Video/app control
            case 'C':
              ovr_RecenterTrackingOrigin(session);
              break;
            case ' ':
              if (pause) {
                if (!started) {
                  started = true;
                  menu.startFadeOut();
                } else {
                  pause = false;
                  startTime =
                      getCurrentTime() - std::chrono::milliseconds((int)getVideoTimeMs(videoFile));
                  soundtrack.play();
                }
              } else {
                pause = true;
                soundtrack.pause();
              }
              break;
            case 'H':
              fade = fade ? 0 : kHeadboxFade;
              break;
            case 'B':
              scene.renderBackground = !scene.renderBackground;
              break;
          }
        }

        if (!sessionStatus.HmdMounted) {
          pause = true;
          soundtrack.pause();
        }

        // Call ovr_GetRenderDesc each frame to get the ovrEyeRenderDesc, as the returned values
        // (e.g. HmdToEyeOffset) may change at runtime.
        ovrEyeRenderDesc eyeRenderDesc[2];
        eyeRenderDesc[0] = ovr_GetRenderDesc(session, ovrEye_Left, hmdDesc.DefaultEyeFov[0]);
        eyeRenderDesc[1] = ovr_GetRenderDesc(session, ovrEye_Right, hmdDesc.DefaultEyeFov[1]);

        // Get eye poses, feeding in correct IPD offset
        ovrPosef EyeRenderPose[2];
        ovrPosef HmdToEyePose[2] = {eyeRenderDesc[0].HmdToEyePose, eyeRenderDesc[1].HmdToEyePose};

        double sensorSampleTime; // sensorSampleTime is fed into the layer later
        ovr_GetEyePoses(
            session, frameIndex, ovrTrue, HmdToEyePose, EyeRenderPose, &sensorSampleTime);

        menu.update();
        soundtrack.updatePositionalTracking(EyeRenderPose[0]);

        // Sync audio and video
        bool delayNextFrame = false;
        if (menu.isHidden && !pause) {
          if (videoFile.getFront() == 0) {
            startTime = getCurrentTime();
            soundtrack.restart(); // video is at beginning, restart audio
          } else {
            const float audioTimeMs = soundtrack.getElapsedMs();
            const float elapsedTimeMs =
                std::chrono::duration<float, std::milli>(getCurrentTime() - startTime).count();
            const bool useAudioTimeAsReference = soundtrack.isPlaying();
            const float referenceTimeMs = useAudioTimeAsReference ? audioTimeMs : elapsedTimeMs;

            // 90 ms is the acceptability threshold (Rec. ITU-R BT.1359-1)
            static const float kMaxVideoLag = 90;
            static const float kMaxAudioLag = 5;
            const float videoTimeMs = getVideoTimeMs(videoFile);
            if (videoTimeMs > referenceTimeMs + kMaxAudioLag) { // video is ahead
              // Delay if we have no audio or if we're using audio and it has started
              if (!soundtrack.isPlaying() || audioTimeMs != 0) {
                delayNextFrame = true;
              }
            } else if (referenceTimeMs > videoTimeMs + kMaxVideoLag) { // video is behind
              // Stuttering is worse than de-sync, as long as it catches up, so do nothing;
              // alternatively, we can stutter to realign via soundtrack.setElapsedMs(videoTimeMs)
            }
          }
        }

        if (!delayNextFrame && !pause && videoFile.frames.size() > 1) {
          // destroy previous frame, finish loading current frame, kick off next frame
          scene.destroyFrame(scene.subframes);
          scene.subframes = videoFile.readEnd(scene);
          videoFile.readBegin(scene, true);
        }

        // Render Scene to Eye Buffers
        for (int eye = 0; eye < 2; ++eye) {
          // Switch to eye render target
          eyeRenderTexture[eye]->SetAndClearRenderSurface();

          // Get view and projection matrices
          Matrix4f rollPitchYaw = Matrix4f::RotationY(Yaw);
          Matrix4f finalRollPitchYaw = rollPitchYaw * Matrix4f(EyeRenderPose[eye].Orientation);
          Vector3f finalUp = finalRollPitchYaw.Transform(Vector3f(0, 1, 0));
          Vector3f finalForward = finalRollPitchYaw.Transform(Vector3f(0, 0, -1));
          Vector3f shiftedEyePos = Pos2 + rollPitchYaw.Transform(EyeRenderPose[eye].Position);

          Matrix4f view = Matrix4f::LookAtRH(shiftedEyePos, shiftedEyePos + finalForward, finalUp);
          Matrix4f proj = ovrMatrix4f_Projection(
              hmdDesc.DefaultEyeFov[eye], 0.2f, 30000.0f, ovrProjection_None);

          // Render world
          Matrix4f projView = proj * view;
          using ForeignType = const Eigen::Matrix<float, 4, 4, Eigen::RowMajor>;

          if (menu.isHidden) {
            const float displacement = fade * Vector3f(EyeRenderPose[0].Position).Length();
            scene.render(Eigen::Map<ForeignType>(projView.M[0]), displacement);
          } else {
            menu.draw(view, proj);
          }

          // Avoids an error when calling SetAndClearRenderSurface during next iteration.
          // Without this, during the next while loop iteration SetAndClearRenderSurface
          // would bind a framebuffer with an invalid COLOR_ATTACHMENT0 because the texture ID
          // associated with COLOR_ATTACHMENT0 had been unlocked by calling wglDXUnlockObjectsNV.
          eyeRenderTexture[eye]->UnsetRenderSurface();

          // Commit changes to the textures so they get picked up frame
          eyeRenderTexture[eye]->Commit();
        }

        // Do distortion rendering, Present and flush/sync

        ovrLayerEyeFovDepth ld;
        ld.Header.Type = ovrLayerType_EyeFov;
        ld.Header.Flags = ovrLayerFlag_TextureOriginAtBottomLeft; // Because OpenGL.

        for (int eye = 0; eye < 2; ++eye) {
          ld.ColorTexture[eye] = eyeRenderTexture[eye]->ColorTextureChain;
          ld.DepthTexture[eye] = eyeRenderTexture[eye]->DepthTextureChain;
          ld.Viewport[eye] = Recti(eyeRenderTexture[eye]->GetSize());
          ld.Fov[eye] = hmdDesc.DefaultEyeFov[eye];
          ld.RenderPose[eye] = EyeRenderPose[eye];
          ld.SensorSampleTime = sensorSampleTime;
        }

        ovrLayerHeader* layers = &ld.Header;
        result = ovr_SubmitFrame(session, frameIndex, nullptr, &layers, 1);

        // exit the rendering loop if submit returns an error, will retry on ovrError_DisplayLost
        if (!OVR_SUCCESS(result))
          goto Done;

        frameIndex++;
      }

      // Blit mirror texture to back buffer
      glBindFramebuffer(GL_READ_FRAMEBUFFER, mirrorFBO);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      GLint w = windowSize.w;
      GLint h = windowSize.h;
      glBlitFramebuffer(0, h, w, 0, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

      SwapBuffers(Platform.hDC);
    }
  }

Done:
  if (mirrorFBO)
    glDeleteFramebuffers(1, &mirrorFBO);
  if (mirrorTexture)
    ovr_DestroyMirrorTexture(session, mirrorTexture);
  for (int eye = 0; eye < 2; ++eye) {
    delete eyeRenderTexture[eye];
  }
  Platform.ReleaseDevice();
  ovr_Destroy(session);

  // Retry on ovrError_DisplayLost
  return retryCreate || (result == ovrError_DisplayLost);
}

int main(int argc, char* argv[]) {
  FLAGS_stderrthreshold = 0;
  FLAGS_logtostderr = 0;

  system_util::initDep(argc, argv, kUsageMessage);

  verifyInputs();

  LOG(INFO) << "Starting...";

  ovrInitParams initParams = {
      ovrInit_RequestVersion | ovrInit_FocusAware, OVR_MINOR_VERSION, NULL, 0, 0};
  ovrResult result = ovr_Initialize(&initParams);
  VALIDATE(OVR_SUCCESS(result), "Failed to initialize libOVR.");

  VALIDATE(
      Platform.InitWindow(GetModuleHandle(NULL), L"Facebook360 DEP | 7 DoF Viewer"),
      "Failed to open window.");

  Platform.Run(MainLoop);

  ovr_Shutdown();

  return 0;
}