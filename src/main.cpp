// main_glfw_padviz.cpp
// GLFW + OpenGL fixed pipeline visualization for 2x virtual pads (6-bit each).
// - Two virtual controllers: each has Up/Down/Left/Right/B1/B2 (digital)
// - Rebind any virtual control to keyboard or any joystick/gamepad input (learning mode)
// - Render two pad diagrams and highlight current states
// - Show current bindings on screen in real time
// - Save/Load bindings to a text file
//
// Hotkeys:
//   ESC            : quit
//   F5             : save bindings to "padmap.txt"
//   F9             : load bindings from "padmap.txt"
//   F1 / F2        : select virtual controller 1 / 2 for editing
//   1..6           : select target control (1:Up 2:Down 3:Left 4:Right 5:B1 6:B2)
//   SPACE          : start learning (next input becomes new binding)
//   BACKSPACE      : clear binding for selected control
//   TAB            : cycle selected controller
//
// Notes for learning:
// - When learning is armed, the first detected input wins:
//   - Keyboard: any key press
//   - Gamepad (recognized): any button press OR axis moved beyond threshold
//   - Joystick (raw): any button press OR axis moved beyond threshold
//
// Build examples:
//   macOS (Homebrew glfw):
//     clang++ main_glfw_padviz.cpp -std=c++17 -I/opt/homebrew/include -L/opt/homebrew/lib -lglfw \
//       -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo -o padviz
//
//   Ubuntu:
//     sudo apt-get install libglfw3-dev
//     g++ main_glfw_padviz.cpp -std=c++17 -lglfw -lGL -ldl -lpthread -o padviz
//
//   Windows (MSVC):
//     Link: glfw3.lib + opengl32.lib
//     Ensure glfw3.dll is available beside exe or in PATH.

#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cmath>
static int gFBW = 0;
static int gFBH = 0;

// -----------------------------------------------------------------------------
// Text rendering: stb_easy_font (ASCII-only; no textures; renders quads).
// Vendor file: third_party/stb/stb_easy_font.h (public domain / MIT).
//
// stb_easy_font uses y increasing downward. This program uses a y-up
// orthographic projection for the fixed pipeline UI, so drawText() flips
// y coordinates in the generated vertex buffer.
// -----------------------------------------------------------------------------
#include "stb_easy_font.h"


// -----------------------------------------------------------------------------
// Input system
// -----------------------------------------------------------------------------
static bool gKeyDown[GLFW_KEY_LAST + 1];
static bool gKeyDownPrev[GLFW_KEY_LAST + 1];

static void onError(int code, const char* desc) {
  std::fprintf(stderr, "[glfw error] code=%d desc=%s\n", code, desc ? desc : "(null)");
}

static void onKey(GLFWwindow* w, int key, int scancode, int action, int mods) {
  (void)w; (void)scancode; (void)mods;
  if (key >= 0 && key <= GLFW_KEY_LAST) {
    if (action == GLFW_PRESS) gKeyDown[key] = true;
    else if (action == GLFW_RELEASE) gKeyDown[key] = false;
  }
  if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
    glfwSetWindowShouldClose(w, GLFW_TRUE);
  }
}

static bool keyPressedEdge(int key) {
  if (key < 0 || key > GLFW_KEY_LAST) return false;
  return gKeyDown[key] && !gKeyDownPrev[key];
}

static void updateKeyPrev() {
  std::memcpy(gKeyDownPrev, gKeyDown, sizeof(gKeyDown));
}

enum class BindType {
  None = 0,
  Key,
  GamepadButton,
  GamepadAxisDir,
  JoyButton,
  JoyAxisDir
};

struct Binding {
  BindType type = BindType::None;
  int jid = -1;        // GLFW joystick id (GLFW_JOYSTICK_1..)
  int code = -1;       // key code OR button index OR axis index
  int dir = 0;         // for axis dir: -1 or +1
  float threshold = 0.45f;

  std::string toString() const {
    char buf[256];
    switch (type) {
      case BindType::None:
        return "None";
      case BindType::Key:
        std::snprintf(buf, sizeof(buf), "Key(%d)", code);
        return buf;
      case BindType::GamepadButton:
        std::snprintf(buf, sizeof(buf), "GP(jid=%d) Btn(%d)", jid, code);
        return buf;
      case BindType::GamepadAxisDir:
        std::snprintf(buf, sizeof(buf), "GP(jid=%d) Axis(%d)%s%.2f", jid, code, (dir < 0 ? "<-" : "->"), threshold);
        return buf;
      case BindType::JoyButton:
        std::snprintf(buf, sizeof(buf), "Joy(jid=%d) Btn(%d)", jid, code);
        return buf;
      case BindType::JoyAxisDir:
        std::snprintf(buf, sizeof(buf), "Joy(jid=%d) Axis(%d)%s%.2f", jid, code, (dir < 0 ? "<-" : "->"), threshold);
        return buf;
      default:
        return "Unknown";
    }
  }
};

static bool sampleBinding(const Binding& b) {
  if (b.type == BindType::None) return false;

  if (b.type == BindType::Key) {
    if (b.code < 0 || b.code > GLFW_KEY_LAST) return false;
    return gKeyDown[b.code];
  }

  if (b.jid < GLFW_JOYSTICK_1 || b.jid > GLFW_JOYSTICK_LAST) return false;
  if (!glfwJoystickPresent(b.jid)) return false;

  if (b.type == BindType::GamepadButton || b.type == BindType::GamepadAxisDir) {
    if (!glfwJoystickIsGamepad(b.jid)) return false;
    GLFWgamepadstate st;
    if (!glfwGetGamepadState(b.jid, &st)) return false;

    if (b.type == BindType::GamepadButton) {
      if (b.code < 0 || b.code > GLFW_GAMEPAD_BUTTON_LAST) return false;
      return st.buttons[b.code] == GLFW_PRESS;
    } else {
      if (b.code < 0 || b.code > GLFW_GAMEPAD_AXIS_LAST) return false;
      float v = st.axes[b.code];
      if (b.dir < 0) return v < -b.threshold;
      if (b.dir > 0) return v >  b.threshold;
      return false;
    }
  }

  // Raw joystick
  if (b.type == BindType::JoyButton) {
    int n = 0;
    const unsigned char* btn = glfwGetJoystickButtons(b.jid, &n);
    if (!btn || b.code < 0 || b.code >= n) return false;
    return btn[b.code] == GLFW_PRESS;
  }
  if (b.type == BindType::JoyAxisDir) {
    int n = 0;
    const float* ax = glfwGetJoystickAxes(b.jid, &n);
    if (!ax || b.code < 0 || b.code >= n) return false;
    float v = ax[b.code];
    if (b.dir < 0) return v < -b.threshold;
    if (b.dir > 0) return v >  b.threshold;
    return false;
  }

  return false;
}

struct Digital6 {
  bool up = false;
  bool down = false;
  bool left = false;
  bool right = false;
  bool b1 = false;
  bool b2 = false;

  uint8_t packBits() const {
    // 0=Up,1=Down,2=Left,3=Right,4=B1,5=B2
    uint8_t v = 0;
    v |= up    ? (1u << 0) : 0;
    v |= down  ? (1u << 1) : 0;
    v |= left  ? (1u << 2) : 0;
    v |= right ? (1u << 3) : 0;
    v |= b1    ? (1u << 4) : 0;
    v |= b2    ? (1u << 5) : 0;
    return v;
  }
};

enum class VKey {
  Up = 0,
  Down,
  Left,
  Right,
  B1,
  B2,
  Count
};

static const char* vkeyName(VKey k) {
  switch (k) {
    case VKey::Up: return "Up";
    case VKey::Down: return "Down";
    case VKey::Left: return "Left";
    case VKey::Right: return "Right";
    case VKey::B1: return "B1";
    case VKey::B2: return "B2";
    default: return "?";
  }
}

struct VirtualPad {
  Binding bind[(int)VKey::Count];

  Digital6 sample() const {
    Digital6 d;
    d.up = sampleBinding(bind[(int)VKey::Up]);
    d.down = sampleBinding(bind[(int)VKey::Down]);
    d.left = sampleBinding(bind[(int)VKey::Left]);
    d.right = sampleBinding(bind[(int)VKey::Right]);
    d.b1 = sampleBinding(bind[(int)VKey::B1]);
    d.b2 = sampleBinding(bind[(int)VKey::B2]);
    return d;
  }
};

static VirtualPad gPad[2];

// -----------------------------------------------------------------------------
// Joystick state caching for learning edges
// -----------------------------------------------------------------------------
struct JoyCache {
  bool present = false;
  bool isGamepad = false;
  std::string name;

  std::vector<unsigned char> btnPrev;
  std::vector<float> axisPrev;

  std::vector<unsigned char> btnCur;
  std::vector<float> axisCur;

  GLFWgamepadstate gpPrev{};
  GLFWgamepadstate gpCur{};
  bool gpHasPrev = false;
  bool gpHasCur = false;
};

static JoyCache gJoy[GLFW_JOYSTICK_LAST + 1];

static const char* jidName(int jid) {
  const char* n = glfwGetJoystickName(jid);
  return n ? n : "(unknown)";
}

static void updateJoystickCaches() {
  for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
    JoyCache& jc = gJoy[jid];

    jc.present = glfwJoystickPresent(jid);
    if (!jc.present) {
      jc.isGamepad = false;
      jc.name.clear();
      jc.btnPrev.clear();
      jc.axisPrev.clear();
      jc.btnCur.clear();
      jc.axisCur.clear();
      jc.gpHasPrev = false;
      jc.gpHasCur = false;
      continue;
    }

    jc.isGamepad = glfwJoystickIsGamepad(jid);
    jc.name = jidName(jid);

    // Raw buttons/axes
    int nb = 0, na = 0;
    const unsigned char* btn = glfwGetJoystickButtons(jid, &nb);
    const float* ax = glfwGetJoystickAxes(jid, &na);

    jc.btnPrev = jc.btnCur;
    jc.axisPrev = jc.axisCur;

    jc.btnCur.assign(btn ? btn : nullptr, btn ? btn + nb : nullptr);
    jc.axisCur.assign(ax ? ax : nullptr, ax ? ax + na : nullptr);

    // Gamepad state (if possible)
    jc.gpPrev = jc.gpCur;
    jc.gpHasPrev = jc.gpHasCur;

    jc.gpHasCur = false;
    if (jc.isGamepad) {
      GLFWgamepadstate st;
      if (glfwGetGamepadState(jid, &st)) {
        jc.gpCur = st;
        jc.gpHasCur = true;
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Learning (rebinding) state
// -----------------------------------------------------------------------------
static int gEditPad = 0;                 // 0 or 1
static VKey gEditKey = VKey::Up;         // selected virtual control
static bool gLearning = false;           // armed?
static float gLearnThreshold = 0.55f;    // slightly higher to avoid noise

static void clearBinding(int padIdx, VKey k) {
  gPad[padIdx].bind[(int)k] = Binding{};
}

static void setDefaultBindings() {
  // Controller 1 defaults (keyboard): WASD + J/K
  gPad[0].bind[(int)VKey::Up]    = Binding{BindType::Key, -1, GLFW_KEY_W, 0, 0.0f};
  gPad[0].bind[(int)VKey::Down]  = Binding{BindType::Key, -1, GLFW_KEY_S, 0, 0.0f};
  gPad[0].bind[(int)VKey::Left]  = Binding{BindType::Key, -1, GLFW_KEY_A, 0, 0.0f};
  gPad[0].bind[(int)VKey::Right] = Binding{BindType::Key, -1, GLFW_KEY_D, 0, 0.0f};
  gPad[0].bind[(int)VKey::B1]    = Binding{BindType::Key, -1, GLFW_KEY_J, 0, 0.0f};
  gPad[0].bind[(int)VKey::B2]    = Binding{BindType::Key, -1, GLFW_KEY_K, 0, 0.0f};

  // Controller 2 defaults (keyboard): Arrows + N/M
  gPad[1].bind[(int)VKey::Up]    = Binding{BindType::Key, -1, GLFW_KEY_UP, 0, 0.0f};
  gPad[1].bind[(int)VKey::Down]  = Binding{BindType::Key, -1, GLFW_KEY_DOWN, 0, 0.0f};
  gPad[1].bind[(int)VKey::Left]  = Binding{BindType::Key, -1, GLFW_KEY_LEFT, 0, 0.0f};
  gPad[1].bind[(int)VKey::Right] = Binding{BindType::Key, -1, GLFW_KEY_RIGHT, 0, 0.0f};
  gPad[1].bind[(int)VKey::B1]    = Binding{BindType::Key, -1, GLFW_KEY_N, 0, 0.0f};
  gPad[1].bind[(int)VKey::B2]    = Binding{BindType::Key, -1, GLFW_KEY_M, 0, 0.0f};
}

static bool detectAnyKeyPress(int& outKey) {
  for (int k = 0; k <= GLFW_KEY_LAST; ++k) {
    if (gKeyDown[k] && !gKeyDownPrev[k]) {
      outKey = k;
      return true;
    }
  }
  return false;
}

static bool detectGamepadButtonPress(int& outJid, int& outBtn) {
  for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
    JoyCache& jc = gJoy[jid];
    if (!jc.present || !jc.isGamepad) continue;
    if (!jc.gpHasCur || !jc.gpHasPrev) continue;

    for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b) {
      bool cur = jc.gpCur.buttons[b] == GLFW_PRESS;
      bool prev = jc.gpPrev.buttons[b] == GLFW_PRESS;
      if (cur && !prev) {
        outJid = jid;
        outBtn = b;
        return true;
      }
    }
  }
  return false;
}

static bool detectGamepadAxisMove(int& outJid, int& outAxis, int& outDir) {
  for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
    JoyCache& jc = gJoy[jid];
    if (!jc.present || !jc.isGamepad) continue;
    if (!jc.gpHasCur || !jc.gpHasPrev) continue;

    for (int a = 0; a <= GLFW_GAMEPAD_AXIS_LAST; ++a) {
      float cur = jc.gpCur.axes[a];
      float prev = jc.gpPrev.axes[a];

      // Detect crossing threshold from near zero
      if (std::fabs(prev) < 0.20f) {
        if (cur > gLearnThreshold) {
          outJid = jid; outAxis = a; outDir = +1;
          return true;
        }
        if (cur < -gLearnThreshold) {
          outJid = jid; outAxis = a; outDir = -1;
          return true;
        }
      }
    }
  }
  return false;
}

static bool detectJoyButtonPress(int& outJid, int& outBtn) {
  for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
    JoyCache& jc = gJoy[jid];
    if (!jc.present) continue;

    int n = (int)jc.btnCur.size();
    if ((int)jc.btnPrev.size() != n) continue;

    for (int b = 0; b < n; ++b) {
      bool cur = jc.btnCur[b] == GLFW_PRESS;
      bool prev = jc.btnPrev[b] == GLFW_PRESS;
      if (cur && !prev) {
        outJid = jid;
        outBtn = b;
        return true;
      }
    }
  }
  return false;
}

static bool detectJoyAxisMove(int& outJid, int& outAxis, int& outDir) {
  for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
    JoyCache& jc = gJoy[jid];
    if (!jc.present) continue;

    int n = (int)jc.axisCur.size();
    if ((int)jc.axisPrev.size() != n) continue;

    for (int a = 0; a < n; ++a) {
      float cur = jc.axisCur[a];
      float prev = jc.axisPrev[a];

      if (std::fabs(prev) < 0.20f) {
        if (cur > gLearnThreshold) {
          outJid = jid; outAxis = a; outDir = +1;
          return true;
        }
        if (cur < -gLearnThreshold) {
          outJid = jid; outAxis = a; outDir = -1;
          return true;
        }
      }
    }
  }
  return false;
}

static void applyLearningIfTriggered() {
  if (!gLearning) return;

  Binding b;

  // 1) Keyboard
  int key = -1;
  if (detectAnyKeyPress(key)) {
    b.type = BindType::Key;
    b.code = key;
    b.jid = -1;
    gPad[gEditPad].bind[(int)gEditKey] = b;
    gLearning = false;
    return;
  }

  // 2) Gamepad button/axis
  int jid = -1, code = -1, dir = 0;
  if (detectGamepadButtonPress(jid, code)) {
    b.type = BindType::GamepadButton;
    b.jid = jid;
    b.code = code;
    gPad[gEditPad].bind[(int)gEditKey] = b;
    gLearning = false;
    return;
  }
  if (detectGamepadAxisMove(jid, code, dir)) {
    b.type = BindType::GamepadAxisDir;
    b.jid = jid;
    b.code = code;
    b.dir = dir;
    b.threshold = 0.45f;
    gPad[gEditPad].bind[(int)gEditKey] = b;
    gLearning = false;
    return;
  }

  // 3) Raw joystick button/axis
  if (detectJoyButtonPress(jid, code)) {
    // If it's gamepad, gamepad path already handled above, but raw is still allowed.
    b.type = BindType::JoyButton;
    b.jid = jid;
    b.code = code;
    gPad[gEditPad].bind[(int)gEditKey] = b;
    gLearning = false;
    return;
  }
  if (detectJoyAxisMove(jid, code, dir)) {
    b.type = BindType::JoyAxisDir;
    b.jid = jid;
    b.code = code;
    b.dir = dir;
    b.threshold = 0.45f;
    gPad[gEditPad].bind[(int)gEditKey] = b;
    gLearning = false;
    return;
  }
}

// -----------------------------------------------------------------------------
// Save / Load mapping
// -----------------------------------------------------------------------------
static const char* kMapFile = "padmap.txt";

static bool saveMappings() {
  std::FILE* fp = std::fopen(kMapFile, "wb");
  if (!fp) return false;

  // Format:
  // pad key type jid code dir threshold
  for (int p = 0; p < 2; ++p) {
    for (int k = 0; k < (int)VKey::Count; ++k) {
      const Binding& b = gPad[p].bind[k];
      std::fprintf(fp, "%d %d %d %d %d %d %.6f\n",
        p, k,
        (int)b.type,
        b.jid,
        b.code,
        b.dir,
        b.threshold
      );
    }
  }

  std::fclose(fp);
  return true;
}

static bool loadMappings() {
  std::FILE* fp = std::fopen(kMapFile, "rb");
  if (!fp) return false;

  // Initialize to None first
  for (int p = 0; p < 2; ++p) {
    for (int k = 0; k < (int)VKey::Count; ++k) {
      gPad[p].bind[k] = Binding{};
    }
  }

  int p = 0, k = 0, type = 0, jid = 0, code = 0, dir = 0;
  float th = 0.45f;

  while (true) {
    int r = std::fscanf(fp, "%d %d %d %d %d %d %f", &p, &k, &type, &jid, &code, &dir, &th);
    if (r != 7) break;
    if (p < 0 || p >= 2) continue;
    if (k < 0 || k >= (int)VKey::Count) continue;

    Binding b;
    b.type = (BindType)type;
    b.jid = jid;
    b.code = code;
    b.dir = dir;
    b.threshold = th;
    gPad[p].bind[k] = b;
  }

  std::fclose(fp);
  return true;
}

// -----------------------------------------------------------------------------
// Rendering (fixed pipeline)
// -----------------------------------------------------------------------------
static void setOrtho(int w, int h) {
  gFBW = w;
  gFBH = h;
  glViewport(0, 0, w, h);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, w, 0, h, -1, 1);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
}

static void drawRect(float x0, float y0, float x1, float y1, bool filled) {
  if (filled) {
    glBegin(GL_QUADS);
    glVertex2f(x0, y0);
    glVertex2f(x1, y0);
    glVertex2f(x1, y1);
    glVertex2f(x0, y1);
    glEnd();
  } else {
    glBegin(GL_LINE_LOOP);
    glVertex2f(x0, y0);
    glVertex2f(x1, y0);
    glVertex2f(x1, y1);
    glVertex2f(x0, y1);
    glEnd();
  }
}

static void drawCircle(float cx, float cy, float r, bool filled) {
  const int seg = 32;
  if (filled) glBegin(GL_TRIANGLE_FAN);
  else glBegin(GL_LINE_LOOP);

  if (filled) glVertex2f(cx, cy);
  for (int i = 0; i <= seg; ++i) {
    float a = (float)i / (float)seg * 6.2831853f;
    float x = cx + std::cos(a) * r;
    float y = cy + std::sin(a) * r;
    glVertex2f(x, y);
  }
  glEnd();
}

static void drawText(float x, float y, const char* s, unsigned char r=255, unsigned char g=255, unsigned char b=255, unsigned char a=255) {
  if (!s || !*s) return;

  // stb_easy_font expects y to increase downward. We render with y-up coordinates,
  // so we flip y around the framebuffer height after generating the vertices.
  const float y_down = (float)gFBH - y;

  static alignas(16) unsigned char vbuf[64 * 1024];
  int quads = stb_easy_font_print(x, y_down, (char*)s, nullptr, vbuf, (int)sizeof(vbuf));
  const int verts = quads * 4;

  for (int i = 0; i < verts; ++i) {
    float* py = (float*)(vbuf + i * 16 + 4);
    *py = (float)gFBH - *py;
  }

  glColor4ub(r, g, b, a);
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(2, GL_FLOAT, 16, vbuf);
  glDrawArrays(GL_QUADS, 0, verts);
  glDisableClientState(GL_VERTEX_ARRAY);
}


static void drawPadDiagram(float x, float y, float w, float h, const Digital6& st, const VirtualPad& pad, int padIndex, bool selected) {
  // Body
  glColor3f(0.8f, 0.8f, 0.85f);
  drawRect(x, y, x + w, y + h, true);

  glColor3f(0.2f, 0.2f, 0.25f);
  drawRect(x, y, x + w, y + h, false);

  // Title
  char title[256];
  std::snprintf(title, sizeof(title), "VPad%d  bits=0x%02X  %s", padIndex + 1, st.packBits(), selected ? "[EDIT]" : "");
  drawText(x + 10, y + h - 20, title, 10, 10, 10, 255);

  // D-pad area
  float dpx = x + w * 0.20f;
  float dpy = y + h * 0.50f;
  float dsz = std::min(w, h) * 0.18f;

  auto drawDir = [&](float cx, float cy, float ww, float hh, bool on, const char* label, const char* bindStr) {
    if (on) glColor3f(0.2f, 0.8f, 0.3f);
    else glColor3f(0.6f, 0.6f, 0.6f);
    drawRect(cx - ww*0.5f, cy - hh*0.5f, cx + ww*0.5f, cy + hh*0.5f, true);

    glColor3f(0.2f, 0.2f, 0.25f);
    drawRect(cx - ww*0.5f, cy - hh*0.5f, cx + ww*0.5f, cy + hh*0.5f, false);

    char t[256];
    std::snprintf(t, sizeof(t), "%s: %s", label, bindStr);
    drawText(cx + ww*0.6f, cy - 6, t, 20, 20, 20, 255);
  };

  drawDir(dpx, dpy + dsz, dsz * 0.8f, dsz * 0.6f, st.up, "Up", pad.bind[(int)VKey::Up].toString().c_str());
  drawDir(dpx, dpy - dsz, dsz * 0.8f, dsz * 0.6f, st.down, "Down", pad.bind[(int)VKey::Down].toString().c_str());
  drawDir(dpx - dsz, dpy, dsz * 0.6f, dsz * 0.8f, st.left, "Left", pad.bind[(int)VKey::Left].toString().c_str());
  drawDir(dpx + dsz, dpy, dsz * 0.6f, dsz * 0.8f, st.right, "Right", pad.bind[(int)VKey::Right].toString().c_str());

  // Buttons area
  float bx = x + w * 0.70f;
  float by = y + h * 0.55f;
  float br = std::min(w, h) * 0.06f;

  auto drawBtn = [&](float cx, float cy, bool on, const char* label, const char* bindStr) {
    if (on) glColor3f(0.9f, 0.3f, 0.2f);
    else glColor3f(0.75f, 0.75f, 0.75f);
    drawCircle(cx, cy, br, true);

    glColor3f(0.2f, 0.2f, 0.25f);
    drawCircle(cx, cy, br, false);

    char t[256];
    std::snprintf(t, sizeof(t), "%s: %s", label, bindStr);
    drawText(cx + br * 1.5f, cy - 6, t, 20, 20, 20, 255);
  };

  drawBtn(bx, by + br * 2.0f, st.b1, "B1", pad.bind[(int)VKey::B1].toString().c_str());
  drawBtn(bx, by - br * 2.0f, st.b2, "B2", pad.bind[(int)VKey::B2].toString().c_str());

  // Edit focus highlight
  if (selected) {
    glColor3f(0.1f, 0.4f, 1.0f);
    glLineWidth(3.0f);
    drawRect(x + 2, y + 2, x + w - 2, y + h - 2, false);
    glLineWidth(1.0f);
  }
}

static void drawUIOverlay(int w, int h) {
  char line[512];

  drawText(20, h - 40, "GLFW 2x Virtual Pad (6-bit) - Fixed Pipeline", 240, 240, 240, 255);

  std::snprintf(line, sizeof(line),
    "Edit: pad=%d  target=%s  learning=%s  | F1/F2 pad, 1..6 target, SPACE learn, BACKSPACE clear, F5 save, F9 load",
    gEditPad + 1,
    vkeyName(gEditKey),
    gLearning ? "ON" : "OFF");
  drawText(20, h - 60, line, 220, 220, 220, 255);

  if (gLearning) {
    drawText(20, h - 80, "Learning armed: press a key, or press a pad button, or move an axis.", 255, 220, 120, 255);
  }
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------
static void onJoystick(int jid, int event) {
  if (event == GLFW_CONNECTED) {
    std::fprintf(stderr, "[joy] CONNECT jid=%d name=%s gamepad=%d\n",
      jid, jidName(jid), glfwJoystickIsGamepad(jid) ? 1 : 0);
  } else if (event == GLFW_DISCONNECTED) {
    std::fprintf(stderr, "[joy] DISCONNECT jid=%d\n", jid);
  }
}

static void handleHotkeysOnce() {
  // Save / Load
  if (keyPressedEdge(GLFW_KEY_F5)) {
    bool ok = saveMappings();
    std::fprintf(stderr, ok ? "[map] saved to %s\n" : "[map] save failed\n", kMapFile);
  }
  if (keyPressedEdge(GLFW_KEY_F9)) {
    bool ok = loadMappings();
    std::fprintf(stderr, ok ? "[map] loaded from %s\n" : "[map] load failed\n", kMapFile);
  }

  // Select pad
  if (keyPressedEdge(GLFW_KEY_F1)) gEditPad = 0;
  if (keyPressedEdge(GLFW_KEY_F2)) gEditPad = 1;
  if (keyPressedEdge(GLFW_KEY_TAB)) gEditPad = 1 - gEditPad;

  // Select target control 1..6
  if (keyPressedEdge(GLFW_KEY_1)) gEditKey = VKey::Up;
  if (keyPressedEdge(GLFW_KEY_2)) gEditKey = VKey::Down;
  if (keyPressedEdge(GLFW_KEY_3)) gEditKey = VKey::Left;
  if (keyPressedEdge(GLFW_KEY_4)) gEditKey = VKey::Right;
  if (keyPressedEdge(GLFW_KEY_5)) gEditKey = VKey::B1;
  if (keyPressedEdge(GLFW_KEY_6)) gEditKey = VKey::B2;

  // Start learning
  if (keyPressedEdge(GLFW_KEY_SPACE)) {
    gLearning = true;
  }

  // Clear binding
  if (keyPressedEdge(GLFW_KEY_BACKSPACE)) {
    clearBinding(gEditPad, gEditKey);
    gLearning = false;
  }
}

int main() {
  std::memset(gKeyDown, 0, sizeof(gKeyDown));
  std::memset(gKeyDownPrev, 0, sizeof(gKeyDownPrev));

  glfwSetErrorCallback(onError);
  if (!glfwInit()) {
    std::fprintf(stderr, "glfwInit failed\n");
    return 1;
  }

  // Portable fixed pipeline context
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);

  GLFWwindow* w = glfwCreateWindow(1200, 700, "padviz", nullptr, nullptr);
  if (!w) {
    std::fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return 1;
  }

  glfwMakeContextCurrent(w);
  glfwSwapInterval(1);

  glfwSetKeyCallback(w, onKey);
  glfwSetJoystickCallback(onJoystick);

  setDefaultBindings();
  loadMappings(); // if exists, override defaults

  while (!glfwWindowShouldClose(w)) {
    glfwPollEvents();

    // Update caches
    updateJoystickCaches();

    // Handle hotkeys (edge-based)
    handleHotkeysOnce();

    // Apply learning if armed
    applyLearningIfTriggered();

    // Sample pads
    Digital6 s0 = gPad[0].sample();
    Digital6 s1 = gPad[1].sample();

    // Render
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(w, &fbw, &fbh);

    glDisable(GL_DEPTH_TEST);
    glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    setOrtho(fbw, fbh);

    float padW = fbw * 0.46f;
    float padH = fbh * 0.70f;
    float padY = fbh * 0.12f;

    float pad0X = fbw * 0.04f;
    float pad1X = fbw * 0.50f;

    drawPadDiagram(pad0X, padY, padW, padH, s0, gPad[0], 0, (gEditPad == 0));
    drawPadDiagram(pad1X, padY, padW, padH, s1, gPad[1], 1, (gEditPad == 1));

    drawUIOverlay(fbw, fbh);

    glfwSwapBuffers(w);

    // Update key previous states last
    updateKeyPrev();
  }

  glfwDestroyWindow(w);
  glfwTerminate();
  return 0;
}
