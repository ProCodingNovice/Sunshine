/**
 * @file src/platform/macos/input.cpp
 * @brief Definitions for macOS input handling.
 */
// standard includes
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// platform includes
#include <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <IOKit/hid/IOHIDUserDevice.h>
#include <mach/mach.h>

// local includes
#include "src/display_device.h"
#include "src/input.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

/**
 * @brief Delay for a double click, in milliseconds.
 * @todo Make this configurable.
 */
constexpr std::chrono::milliseconds MULTICLICK_DELAY_MS(500);

namespace platf {
  using namespace std::literals;

  namespace {
    constexpr std::uint8_t HAT_NEUTRAL = 0x0F;
    constexpr std::size_t GAMEPAD_REPORT_SIZE = 9;

    // HID descriptor for a generic 16-button gamepad with a hat and 6 analog axes.
    constexpr std::array<std::uint8_t, 74> GAMEPAD_REPORT_DESCRIPTOR = {
      0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x15, 0x00, 0x25, 0x01, 0x35, 0x00, 0x45, 0x01, 0x75, 0x01, 0x95, 0x10, 0x05, 0x09, 0x19, 0x01,
      0x29, 0x10, 0x81, 0x02, 0x05, 0x01, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x09, 0x39, 0x81,
      0x42, 0x65, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x03, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x05, 0x01, 0x09, 0x30,
      0x09, 0x31, 0x09, 0x33, 0x09, 0x34, 0x09, 0x32, 0x09, 0x35, 0x81, 0x02, 0xC0
    };

    std::array<std::uint8_t, GAMEPAD_REPORT_SIZE> neutral_report() {
      return {0x00, 0x00, HAT_NEUTRAL, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00};
    }

    std::uint8_t axis_from_int16(std::int16_t value) {
      auto normalized = static_cast<int>(value) + 32768;
      return static_cast<std::uint8_t>(normalized >> 8);
    }

    std::uint16_t buttons_from_flags(std::uint32_t flags) {
      std::uint16_t buttons = 0;
      const auto set_bit = [&](std::uint16_t bit, bool enabled) {
        if (enabled) {
          buttons |= static_cast<std::uint16_t>(1u << bit);
        }
      };

      set_bit(0, flags & A);
      set_bit(1, flags & B);
      set_bit(2, flags & X);
      set_bit(3, flags & Y);
      set_bit(4, flags & LEFT_BUTTON);
      set_bit(5, flags & RIGHT_BUTTON);
      set_bit(6, flags & BACK);
      set_bit(7, flags & START);
      set_bit(8, flags & HOME);
      set_bit(9, flags & LEFT_STICK);
      set_bit(10, flags & RIGHT_STICK);
      set_bit(11, flags & TOUCHPAD_BUTTON);
      set_bit(12, flags & MISC_BUTTON);
      set_bit(13, flags & PADDLE1);
      set_bit(14, flags & PADDLE2);
      set_bit(15, flags & PADDLE3);

      return buttons;
    }

    std::uint8_t hat_from_flags(std::uint32_t flags) {
      const bool up = flags & DPAD_UP;
      const bool down = flags & DPAD_DOWN;
      const bool left = flags & DPAD_LEFT;
      const bool right = flags & DPAD_RIGHT;

      if (up && right) {
        return 1;
      }
      if (up && left) {
        return 7;
      }
      if (down && right) {
        return 3;
      }
      if (down && left) {
        return 5;
      }
      if (up) {
        return 0;
      }
      if (right) {
        return 2;
      }
      if (down) {
        return 4;
      }
      if (left) {
        return 6;
      }

      return HAT_NEUTRAL;
    }
  }  // namespace

  struct macos_gamepad_t {
    IOHIDUserDeviceRef device {};
    feedback_queue_t feedback_queue;
    gamepad_id_t id {};
    std::array<std::uint8_t, GAMEPAD_REPORT_SIZE> report = neutral_report();

    ~macos_gamepad_t() {
      if (device) {
        CFRelease(device);
      }
    }
  };

  struct macos_input_t {
  public:
    CGDirectDisplayID display {};
    CGFloat displayScaling {};
    CGEventSourceRef source {};

    // keyboard related stuff
    CGEventRef kb_event {};
    CGEventFlags kb_flags {};

    // mouse related stuff
    CGEventRef mouse_event {};  // mouse event source
    bool mouse_down[3] {};  // mouse button status
    std::chrono::steady_clock::steady_clock::time_point last_mouse_event[3][2];  // timestamp of last mouse events

    std::array<std::unique_ptr<macos_gamepad_t>, MAX_GAMEPADS> gamepads {};
    bool hid_user_device_checked {};
    bool hid_user_device_supported {};
  };

  namespace {
    CFMutableDictionaryRef create_device_properties(const std::string &serial) {
      auto properties = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      if (!properties) {
        return nullptr;
      }

      const auto set_int = [&](CFStringRef key, int value) {
        auto number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
        if (number) {
          CFDictionarySetValue(properties, key, number);
          CFRelease(number);
        }
      };

      set_int(kIOHIDVendorIDKey, 0x054C);
      set_int(kIOHIDProductIDKey, 0x05C4);
      set_int(kIOHIDVersionNumberKey, 0x0001);
      set_int(kIOHIDPrimaryUsagePageKey, kHIDPage_GenericDesktop);
      set_int(kIOHIDPrimaryUsageKey, kHIDUsage_GD_GamePad);

      CFDictionarySetValue(properties, kIOHIDManufacturerKey, CFSTR("Sunshine"));
      CFDictionarySetValue(properties, kIOHIDProductKey, CFSTR("Sunshine Virtual Gamepad"));

      if (!serial.empty()) {
        auto serial_cf = CFStringCreateWithCString(kCFAllocatorDefault, serial.c_str(), kCFStringEncodingUTF8);
        if (serial_cf) {
          CFDictionarySetValue(properties, kIOHIDSerialNumberKey, serial_cf);
          CFRelease(serial_cf);
        }
      }

      auto descriptor = CFDataCreate(kCFAllocatorDefault, GAMEPAD_REPORT_DESCRIPTOR.data(), GAMEPAD_REPORT_DESCRIPTOR.size());
      if (descriptor) {
        CFDictionarySetValue(properties, kIOHIDReportDescriptorKey, descriptor);
        CFRelease(descriptor);
      }

      return properties;
    }

    bool ensure_virtual_gamepad_support(macos_input_t &input) {
      if (input.hid_user_device_checked) {
        return input.hid_user_device_supported;
      }

      input.hid_user_device_checked = true;
      auto props = create_device_properties("sunshine-probe");
      if (!props) {
        input.hid_user_device_supported = false;
        return false;
      }

      auto device = IOHIDUserDeviceCreate(kCFAllocatorDefault, props);
      CFRelease(props);

      if (!device) {
        BOOST_LOG(error) << "Unable to create IOHIDUserDevice for Sunshine gamepad"sv;
        input.hid_user_device_supported = false;
        return false;
      }

      CFRelease(device);
      input.hid_user_device_supported = true;
      return true;
    }

    macos_gamepad_t *get_gamepad(macos_input_t &input, int index) {
      if (index < 0 || index >= static_cast<int>(input.gamepads.size())) {
        return nullptr;
      }
      return input.gamepads[index].get();
    }

    void submit_report(macos_gamepad_t &gamepad) {
      if (!gamepad.device) {
        return;
      }

      auto status = IOHIDUserDeviceHandleReport(gamepad.device, gamepad.report.data(), gamepad.report.size());
      if (status != kIOReturnSuccess) {
        BOOST_LOG(warning) << "Failed to submit HID report ["sv << util::hex(static_cast<std::uint32_t>(status)).to_string_view() << ']';
      }
    }

    void populate_report_from_state(macos_gamepad_t &gamepad, const gamepad_state_t &state) {
      const auto buttons = buttons_from_flags(state.buttonFlags);
      gamepad.report[0] = static_cast<std::uint8_t>(buttons & 0xFF);
      gamepad.report[1] = static_cast<std::uint8_t>((buttons >> 8) & 0xFF);
      gamepad.report[2] = hat_from_flags(state.buttonFlags);
      gamepad.report[3] = axis_from_int16(state.lsX);
      gamepad.report[4] = axis_from_int16(state.lsY);
      gamepad.report[5] = axis_from_int16(state.rsX);
      gamepad.report[6] = axis_from_int16(state.rsY);
      gamepad.report[7] = state.lt;
      gamepad.report[8] = state.rt;
    }
  }  // namespace

  // A struct to hold a Windows keycode to Mac virtual keycode mapping.
  struct KeyCodeMap {
    int win_keycode;
    int mac_keycode;
  };

  // Customized less operator for using std::lower_bound() on a KeyCodeMap array.
  bool operator<(const KeyCodeMap &a, const KeyCodeMap &b) {
    return a.win_keycode < b.win_keycode;
  }

  // clang-format off
const KeyCodeMap kKeyCodesMap[] = {
  { 0x08 /* VKEY_BACK */,                      kVK_Delete              },
  { 0x09 /* VKEY_TAB */,                       kVK_Tab                 },
  { 0x0A /* VKEY_BACKTAB */,                   0x21E4                  },
  { 0x0C /* VKEY_CLEAR */,                     kVK_ANSI_KeypadClear    },
  { 0x0D /* VKEY_RETURN */,                    kVK_Return              },
  { 0x10 /* VKEY_SHIFT */,                     kVK_Shift               },
  { 0x11 /* VKEY_CONTROL */,                   kVK_Control             },
  { 0x12 /* VKEY_MENU */,                      kVK_Option              },
  { 0x13 /* VKEY_PAUSE */,                     -1                      },
  { 0x14 /* VKEY_CAPITAL */,                   kVK_CapsLock            },
  { 0x15 /* VKEY_KANA */,                      kVK_JIS_Kana            },
  { 0x15 /* VKEY_HANGUL */,                    -1                      },
  { 0x17 /* VKEY_JUNJA */,                     -1                      },
  { 0x18 /* VKEY_FINAL */,                     -1                      },
  { 0x19 /* VKEY_HANJA */,                     -1                      },
  { 0x19 /* VKEY_KANJI */,                     -1                      },
  { 0x1B /* VKEY_ESCAPE */,                    kVK_Escape              },
  { 0x1C /* VKEY_CONVERT */,                   -1                      },
  { 0x1D /* VKEY_NONCONVERT */,                -1                      },
  { 0x1E /* VKEY_ACCEPT */,                    -1                      },
  { 0x1F /* VKEY_MODECHANGE */,                -1                      },
  { 0x20 /* VKEY_SPACE */,                     kVK_Space               },
  { 0x21 /* VKEY_PRIOR */,                     kVK_PageUp              },
  { 0x22 /* VKEY_NEXT */,                      kVK_PageDown            },
  { 0x23 /* VKEY_END */,                       kVK_End                 },
  { 0x24 /* VKEY_HOME */,                      kVK_Home                },
  { 0x25 /* VKEY_LEFT */,                      kVK_LeftArrow           },
  { 0x26 /* VKEY_UP */,                        kVK_UpArrow             },
  { 0x27 /* VKEY_RIGHT */,                     kVK_RightArrow          },
  { 0x28 /* VKEY_DOWN */,                      kVK_DownArrow           },
  { 0x29 /* VKEY_SELECT */,                    -1                      },
  { 0x2A /* VKEY_PRINT */,                     -1                      },
  { 0x2B /* VKEY_EXECUTE */,                   -1                      },
  { 0x2C /* VKEY_SNAPSHOT */,                  -1                      },
  { 0x2D /* VKEY_INSERT */,                    kVK_Help                },
  { 0x2E /* VKEY_DELETE */,                    kVK_ForwardDelete       },
  { 0x2F /* VKEY_HELP */,                      kVK_Help                },
  { 0x30 /* VKEY_0 */,                         kVK_ANSI_0              },
  { 0x31 /* VKEY_1 */,                         kVK_ANSI_1              },
  { 0x32 /* VKEY_2 */,                         kVK_ANSI_2              },
  { 0x33 /* VKEY_3 */,                         kVK_ANSI_3              },
  { 0x34 /* VKEY_4 */,                         kVK_ANSI_4              },
  { 0x35 /* VKEY_5 */,                         kVK_ANSI_5              },
  { 0x36 /* VKEY_6 */,                         kVK_ANSI_6              },
  { 0x37 /* VKEY_7 */,                         kVK_ANSI_7              },
  { 0x38 /* VKEY_8 */,                         kVK_ANSI_8              },
  { 0x39 /* VKEY_9 */,                         kVK_ANSI_9              },
  { 0x41 /* VKEY_A */,                         kVK_ANSI_A              },
  { 0x42 /* VKEY_B */,                         kVK_ANSI_B              },
  { 0x43 /* VKEY_C */,                         kVK_ANSI_C              },
  { 0x44 /* VKEY_D */,                         kVK_ANSI_D              },
  { 0x45 /* VKEY_E */,                         kVK_ANSI_E              },
  { 0x46 /* VKEY_F */,                         kVK_ANSI_F              },
  { 0x47 /* VKEY_G */,                         kVK_ANSI_G              },
  { 0x48 /* VKEY_H */,                         kVK_ANSI_H              },
  { 0x49 /* VKEY_I */,                         kVK_ANSI_I              },
  { 0x4A /* VKEY_J */,                         kVK_ANSI_J              },
  { 0x4B /* VKEY_K */,                         kVK_ANSI_K              },
  { 0x4C /* VKEY_L */,                         kVK_ANSI_L              },
  { 0x4D /* VKEY_M */,                         kVK_ANSI_M              },
  { 0x4E /* VKEY_N */,                         kVK_ANSI_N              },
  { 0x4F /* VKEY_O */,                         kVK_ANSI_O              },
  { 0x50 /* VKEY_P */,                         kVK_ANSI_P              },
  { 0x51 /* VKEY_Q */,                         kVK_ANSI_Q              },
  { 0x52 /* VKEY_R */,                         kVK_ANSI_R              },
  { 0x53 /* VKEY_S */,                         kVK_ANSI_S              },
  { 0x54 /* VKEY_T */,                         kVK_ANSI_T              },
  { 0x55 /* VKEY_U */,                         kVK_ANSI_U              },
  { 0x56 /* VKEY_V */,                         kVK_ANSI_V              },
  { 0x57 /* VKEY_W */,                         kVK_ANSI_W              },
  { 0x58 /* VKEY_X */,                         kVK_ANSI_X              },
  { 0x59 /* VKEY_Y */,                         kVK_ANSI_Y              },
  { 0x5A /* VKEY_Z */,                         kVK_ANSI_Z              },
  { 0x5B /* VKEY_LWIN */,                      kVK_Command             },
  { 0x5C /* VKEY_RWIN */,                      kVK_RightCommand        },
  { 0x5D /* VKEY_APPS */,                      kVK_RightCommand        },
  { 0x5F /* VKEY_SLEEP */,                     -1                      },
  { 0x60 /* VKEY_NUMPAD0 */,                   kVK_ANSI_Keypad0        },
  { 0x61 /* VKEY_NUMPAD1 */,                   kVK_ANSI_Keypad1        },
  { 0x62 /* VKEY_NUMPAD2 */,                   kVK_ANSI_Keypad2        },
  { 0x63 /* VKEY_NUMPAD3 */,                   kVK_ANSI_Keypad3        },
  { 0x64 /* VKEY_NUMPAD4 */,                   kVK_ANSI_Keypad4        },
  { 0x65 /* VKEY_NUMPAD5 */,                   kVK_ANSI_Keypad5        },
  { 0x66 /* VKEY_NUMPAD6 */,                   kVK_ANSI_Keypad6        },
  { 0x67 /* VKEY_NUMPAD7 */,                   kVK_ANSI_Keypad7        },
  { 0x68 /* VKEY_NUMPAD8 */,                   kVK_ANSI_Keypad8        },
  { 0x69 /* VKEY_NUMPAD9 */,                   kVK_ANSI_Keypad9        },
  { 0x6A /* VKEY_MULTIPLY */,                  kVK_ANSI_KeypadMultiply },
  { 0x6B /* VKEY_ADD */,                       kVK_ANSI_KeypadPlus     },
  { 0x6C /* VKEY_SEPARATOR */,                 -1                      },
  { 0x6D /* VKEY_SUBTRACT */,                  kVK_ANSI_KeypadMinus    },
  { 0x6E /* VKEY_DECIMAL */,                   kVK_ANSI_KeypadDecimal  },
  { 0x6F /* VKEY_DIVIDE */,                    kVK_ANSI_KeypadDivide   },
  { 0x70 /* VKEY_F1 */,                        kVK_F1                  },
  { 0x71 /* VKEY_F2 */,                        kVK_F2                  },
  { 0x72 /* VKEY_F3 */,                        kVK_F3                  },
  { 0x73 /* VKEY_F4 */,                        kVK_F4                  },
  { 0x74 /* VKEY_F5 */,                        kVK_F5                  },
  { 0x75 /* VKEY_F6 */,                        kVK_F6                  },
  { 0x76 /* VKEY_F7 */,                        kVK_F7                  },
  { 0x77 /* VKEY_F8 */,                        kVK_F8                  },
  { 0x78 /* VKEY_F9 */,                        kVK_F9                  },
  { 0x79 /* VKEY_F10 */,                       kVK_F10                 },
  { 0x7A /* VKEY_F11 */,                       kVK_F11                 },
  { 0x7B /* VKEY_F12 */,                       kVK_F12                 },
  { 0x7C /* VKEY_F13 */,                       kVK_F13                 },
  { 0x7D /* VKEY_F14 */,                       kVK_F14                 },
  { 0x7E /* VKEY_F15 */,                       kVK_F15                 },
  { 0x7F /* VKEY_F16 */,                       kVK_F16                 },
  { 0x80 /* VKEY_F17 */,                       kVK_F17                 },
  { 0x81 /* VKEY_F18 */,                       kVK_F18                 },
  { 0x82 /* VKEY_F19 */,                       kVK_F19                 },
  { 0x83 /* VKEY_F20 */,                       kVK_F20                 },
  { 0x84 /* VKEY_F21 */,                       -1                      },
  { 0x85 /* VKEY_F22 */,                       -1                      },
  { 0x86 /* VKEY_F23 */,                       -1                      },
  { 0x87 /* VKEY_F24 */,                       -1                      },
  { 0x90 /* VKEY_NUMLOCK */,                   -1                      },
  { 0x91 /* VKEY_SCROLL */,                    -1                      },
  { 0xA0 /* VKEY_LSHIFT */,                    kVK_Shift               },
  { 0xA1 /* VKEY_RSHIFT */,                    kVK_RightShift          },
  { 0xA2 /* VKEY_LCONTROL */,                  kVK_Control             },
  { 0xA3 /* VKEY_RCONTROL */,                  kVK_RightControl        },
  { 0xA4 /* VKEY_LMENU */,                     kVK_Option              },
  { 0xA5 /* VKEY_RMENU */,                     kVK_RightOption         },
  { 0xA6 /* VKEY_BROWSER_BACK */,              -1                      },
  { 0xA7 /* VKEY_BROWSER_FORWARD */,           -1                      },
  { 0xA8 /* VKEY_BROWSER_REFRESH */,           -1                      },
  { 0xA9 /* VKEY_BROWSER_STOP */,              -1                      },
  { 0xAA /* VKEY_BROWSER_SEARCH */,            -1                      },
  { 0xAB /* VKEY_BROWSER_FAVORITES */,         -1                      },
  { 0xAC /* VKEY_BROWSER_HOME */,              -1                      },
  { 0xAD /* VKEY_VOLUME_MUTE */,               -1                      },
  { 0xAE /* VKEY_VOLUME_DOWN */,               -1                      },
  { 0xAF /* VKEY_VOLUME_UP */,                 -1                      },
  { 0xB0 /* VKEY_MEDIA_NEXT_TRACK */,          -1                      },
  { 0xB1 /* VKEY_MEDIA_PREV_TRACK */,          -1                      },
  { 0xB2 /* VKEY_MEDIA_STOP */,                -1                      },
  { 0xB3 /* VKEY_MEDIA_PLAY_PAUSE */,          -1                      },
  { 0xB4 /* VKEY_MEDIA_LAUNCH_MAIL */,         -1                      },
  { 0xB5 /* VKEY_MEDIA_LAUNCH_MEDIA_SELECT */, -1                      },
  { 0xB6 /* VKEY_MEDIA_LAUNCH_APP1 */,         -1                      },
  { 0xB7 /* VKEY_MEDIA_LAUNCH_APP2 */,         -1                      },
  { 0xBA /* VKEY_OEM_1 */,                     kVK_ANSI_Semicolon      },
  { 0xBB /* VKEY_OEM_PLUS */,                  kVK_ANSI_Equal          },
  { 0xBC /* VKEY_OEM_COMMA */,                 kVK_ANSI_Comma          },
  { 0xBD /* VKEY_OEM_MINUS */,                 kVK_ANSI_Minus          },
  { 0xBE /* VKEY_OEM_PERIOD */,                kVK_ANSI_Period         },
  { 0xBF /* VKEY_OEM_2 */,                     kVK_ANSI_Slash          },
  { 0xC0 /* VKEY_OEM_3 */,                     kVK_ANSI_Grave          },
  { 0xDB /* VKEY_OEM_4 */,                     kVK_ANSI_LeftBracket    },
  { 0xDC /* VKEY_OEM_5 */,                     kVK_ANSI_Backslash      },
  { 0xDD /* VKEY_OEM_6 */,                     kVK_ANSI_RightBracket   },
  { 0xDE /* VKEY_OEM_7 */,                     kVK_ANSI_Quote          },
  { 0xDF /* VKEY_OEM_8 */,                     -1                      },
  { 0xE2 /* VKEY_OEM_102 */,                   -1                      },
  { 0xE5 /* VKEY_PROCESSKEY */,                -1                      },
  { 0xE7 /* VKEY_PACKET */,                    -1                      },
  { 0xF6 /* VKEY_ATTN */,                      -1                      },
  { 0xF7 /* VKEY_CRSEL */,                     -1                      },
  { 0xF8 /* VKEY_EXSEL */,                     -1                      },
  { 0xF9 /* VKEY_EREOF */,                     -1                      },
  { 0xFA /* VKEY_PLAY */,                      -1                      },
  { 0xFB /* VKEY_ZOOM */,                      -1                      },
  { 0xFC /* VKEY_NONAME */,                    -1                      },
  { 0xFD /* VKEY_PA1 */,                       -1                      },
  { 0xFE /* VKEY_OEM_CLEAR */,                 kVK_ANSI_KeypadClear    }
};
  // clang-format on

  int keysym(int keycode) {
    KeyCodeMap key_map {};

    key_map.win_keycode = keycode;
    const KeyCodeMap *temp_map = std::lower_bound(
      kKeyCodesMap,
      kKeyCodesMap + sizeof(kKeyCodesMap) / sizeof(kKeyCodesMap[0]),
      key_map
    );

    if (temp_map >= kKeyCodesMap + sizeof(kKeyCodesMap) / sizeof(kKeyCodesMap[0]) ||
        temp_map->win_keycode != keycode || temp_map->mac_keycode == -1) {
      return -1;
    }

    return temp_map->mac_keycode;
  }

  void keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags) {
    auto key = keysym(modcode);

    BOOST_LOG(debug) << "got keycode: 0x"sv << std::hex << modcode << ", translated to: 0x" << std::hex << key << ", release:" << release;

    if (key < 0) {
      return;
    }

    auto macos_input = ((macos_input_t *) input.get());
    auto event = macos_input->kb_event;

    if (key == kVK_Shift || key == kVK_RightShift ||
        key == kVK_Command || key == kVK_RightCommand ||
        key == kVK_Option || key == kVK_RightOption ||
        key == kVK_Control || key == kVK_RightControl) {
      CGEventFlags mask;

      switch (key) {
        case kVK_Shift:
        case kVK_RightShift:
          mask = kCGEventFlagMaskShift;
          break;
        case kVK_Command:
        case kVK_RightCommand:
          mask = kCGEventFlagMaskCommand;
          break;
        case kVK_Option:
        case kVK_RightOption:
          mask = kCGEventFlagMaskAlternate;
          break;
        case kVK_Control:
        case kVK_RightControl:
          mask = kCGEventFlagMaskControl;
          break;
      }

      macos_input->kb_flags = release ? macos_input->kb_flags & ~mask : macos_input->kb_flags | mask;
      CGEventSetType(event, kCGEventFlagsChanged);
      CGEventSetFlags(event, macos_input->kb_flags);
    } else {
      CGEventSetIntegerValueField(event, kCGKeyboardEventKeycode, key);
      CGEventSetType(event, release ? kCGEventKeyUp : kCGEventKeyDown);
    }

    CGEventPost(kCGHIDEventTap, event);
  }

  void unicode(input_t &input, char *utf8, int size) {
    BOOST_LOG(info) << "unicode: Unicode input not yet implemented for MacOS."sv;
  }

  int alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &, feedback_queue_t feedback_queue) {
    auto macos_input = static_cast<macos_input_t *>(input.get());
    if (!macos_input) {
      return -1;
    }

    if (!ensure_virtual_gamepad_support(*macos_input)) {
      BOOST_LOG(error) << "alloc_gamepad: IOHID virtual devices are unavailable on this system"sv;
      return -1;
    }

    if (id.globalIndex < 0 || id.globalIndex >= static_cast<int>(macos_input->gamepads.size())) {
      BOOST_LOG(warning) << "alloc_gamepad: Invalid index ["sv << id.globalIndex << ']';
      return -1;
    }

    auto &slot = macos_input->gamepads[id.globalIndex];
    if (slot) {
      BOOST_LOG(warning) << "alloc_gamepad: slot already in use ["sv << id.globalIndex << ']';
      return -1;
    }

    auto serial = "sunshine-gp-"s + std::to_string(id.globalIndex);
    auto props = create_device_properties(serial);
    if (!props) {
      BOOST_LOG(error) << "alloc_gamepad: Failed to build device properties"sv;
      return -1;
    }

    auto device = IOHIDUserDeviceCreate(kCFAllocatorDefault, props);
    CFRelease(props);
    if (!device) {
      BOOST_LOG(error) << "alloc_gamepad: IOHIDUserDeviceCreate failed"sv;
      return -1;
    }

    slot = std::make_unique<macos_gamepad_t>();
    slot->device = device;
    slot->feedback_queue = std::move(feedback_queue);
    slot->id = id;
    slot->report = neutral_report();
    submit_report(*slot);

    BOOST_LOG(info) << "Allocated Sunshine virtual gamepad slot ["sv << id.globalIndex << ']';
    return 0;
  }

  void free_gamepad(input_t &input, int nr) {
    auto macos_input = static_cast<macos_input_t *>(input.get());
    if (!macos_input || nr < 0 || nr >= static_cast<int>(macos_input->gamepads.size())) {
      return;
    }

    auto &slot = macos_input->gamepads[nr];
    if (!slot) {
      return;
    }

    slot->report = neutral_report();
    submit_report(*slot);
    slot.reset();
  }

  void gamepad_update(input_t &input, int nr, const gamepad_state_t &gamepad_state) {
    auto macos_input = static_cast<macos_input_t *>(input.get());
    if (!macos_input) {
      return;
    }

    auto slot = get_gamepad(*macos_input, nr);
    if (!slot) {
      return;
    }

    populate_report_from_state(*slot, gamepad_state);
    submit_report(*slot);
  }

  // returns current mouse location:
  util::point_t get_mouse_loc(input_t &input) {
    // Creating a new event every time to avoid any reuse risk
    const auto macos_input = static_cast<macos_input_t *>(input.get());
    const auto snapshot_event = CGEventCreate(macos_input->source);
    const auto current = CGEventGetLocation(snapshot_event);
    CFRelease(snapshot_event);
    return util::point_t {
      current.x,
      current.y
    };
  }

  void post_mouse(
    input_t &input,
    const CGMouseButton button,
    const CGEventType type,
    const util::point_t raw_location,
    const util::point_t previous_location,
    const int click_count
  ) {
    BOOST_LOG(debug) << "mouse_event: "sv << button << ", type: "sv << type << ", location:"sv << raw_location.x << ":"sv << raw_location.y << " click_count: "sv << click_count;

    const auto macos_input = static_cast<macos_input_t *>(input.get());
    const auto display = macos_input->display;
    const auto event = macos_input->mouse_event;

    // get display bounds for current display
    const CGRect display_bounds = CGDisplayBounds(display);

    // limit mouse to current display bounds
    const auto location = CGPoint {
      std::clamp(raw_location.x, display_bounds.origin.x, display_bounds.origin.x + display_bounds.size.width - 1),
      std::clamp(raw_location.y, display_bounds.origin.y, display_bounds.origin.y + display_bounds.size.height - 1)
    };

    CGEventSetType(event, type);
    CGEventSetLocation(event, location);
    CGEventSetIntegerValueField(event, kCGMouseEventButtonNumber, button);
    CGEventSetIntegerValueField(event, kCGMouseEventClickState, click_count);

    // Include deltas so some 3D applications can consume changes (game cameras, etc)
    const double deltaX = raw_location.x - previous_location.x;
    const double deltaY = raw_location.y - previous_location.y;
    CGEventSetDoubleValueField(event, kCGMouseEventDeltaX, deltaX);
    CGEventSetDoubleValueField(event, kCGMouseEventDeltaY, deltaY);

    CGEventPost(kCGHIDEventTap, event);
    // For why this is here, see:
    // https://stackoverflow.com/questions/15194409/simulated-mouseevent-not-working-properly-osx
    CGWarpMouseCursorPosition(location);
  }

  inline CGEventType event_type_mouse(input_t &input) {
    const auto macos_input = static_cast<macos_input_t *>(input.get());

    if (macos_input->mouse_down[0]) {
      return kCGEventLeftMouseDragged;
    }
    if (macos_input->mouse_down[1]) {
      return kCGEventOtherMouseDragged;
    }
    if (macos_input->mouse_down[2]) {
      return kCGEventRightMouseDragged;
    }
    return kCGEventMouseMoved;
  }

  void move_mouse(
    input_t &input,
    const int deltaX,
    const int deltaY
  ) {
    const auto current = get_mouse_loc(input);

    const auto location = util::point_t {current.x + deltaX, current.y + deltaY};
    post_mouse(input, kCGMouseButtonLeft, event_type_mouse(input), location, current, 0);
  }

  void abs_mouse(
    input_t &input,
    const touch_port_t &touch_port,
    const float x,
    const float y
  ) {
    const auto macos_input = static_cast<macos_input_t *>(input.get());
    const auto scaling = macos_input->displayScaling;
    const auto display = macos_input->display;

    auto location = util::point_t {x * scaling, y * scaling};
    CGRect display_bounds = CGDisplayBounds(display);
    // in order to get the correct mouse location for capturing display , we need to add the display bounds to the location
    location.x += display_bounds.origin.x;
    location.y += display_bounds.origin.y;

    post_mouse(input, kCGMouseButtonLeft, event_type_mouse(input), location, get_mouse_loc(input), 0);
  }

  void button_mouse(input_t &input, const int button, const bool release) {
    CGMouseButton mac_button;
    CGEventType event;

    const auto macos_input = static_cast<macos_input_t *>(input.get());

    switch (button) {
      case 1:
        mac_button = kCGMouseButtonLeft;
        event = release ? kCGEventLeftMouseUp : kCGEventLeftMouseDown;
        break;
      case 2:
        mac_button = kCGMouseButtonCenter;
        event = release ? kCGEventOtherMouseUp : kCGEventOtherMouseDown;
        break;
      case 3:
        mac_button = kCGMouseButtonRight;
        event = release ? kCGEventRightMouseUp : kCGEventRightMouseDown;
        break;
      default:
        BOOST_LOG(warning) << "Unsupported mouse button for MacOS: "sv << button;
        return;
    }

    macos_input->mouse_down[mac_button] = !release;

    // if the last mouse down was less than MULTICLICK_DELAY_MS, we send a double click event
    const auto now = std::chrono::steady_clock::now();
    const auto mouse_position = get_mouse_loc(input);

    if (now < macos_input->last_mouse_event[mac_button][release] + MULTICLICK_DELAY_MS) {
      post_mouse(input, mac_button, event, mouse_position, mouse_position, 2);
    } else {
      post_mouse(input, mac_button, event, mouse_position, mouse_position, 1);
    }

    macos_input->last_mouse_event[mac_button][release] = now;
  }

  void scroll(input_t &input, const int high_res_distance) {
    CGEventRef upEvent = CGEventCreateScrollWheelEvent(
      nullptr,
      kCGScrollEventUnitLine,
      2,
      high_res_distance > 0 ? 1 : -1,
      high_res_distance
    );
    CGEventPost(kCGHIDEventTap, upEvent);
    CFRelease(upEvent);
  }

  void hscroll(input_t &input, int high_res_distance) {
    // Unimplemented
  }

  /**
   * @brief Allocates a context to store per-client input data.
   * @param input The global input context.
   * @return A unique pointer to a per-client input data context.
   */
  std::unique_ptr<client_input_t> allocate_client_input_context(input_t &input) {
    // Unused
    return nullptr;
  }

  /**
   * @brief Sends a touch event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param touch The touch event.
   */
  void touch_update(client_input_t *input, const touch_port_t &touch_port, const touch_input_t &touch) {
    // Unimplemented feature - platform_caps::pen_touch
  }

  /**
   * @brief Sends a pen event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param pen The pen event.
   */
  void pen_update(client_input_t *input, const touch_port_t &touch_port, const pen_input_t &pen) {
    // Unimplemented feature - platform_caps::pen_touch
  }

  /**
   * @brief Sends a gamepad touch event to the OS.
   * @param input The global input context.
   * @param touch The touch event.
   */
  void gamepad_touch(input_t &input, const gamepad_touch_t &touch) {
    // Unimplemented feature - platform_caps::controller_touch
  }

  /**
   * @brief Sends a gamepad motion event to the OS.
   * @param input The global input context.
   * @param motion The motion event.
   */
  void gamepad_motion(input_t &input, const gamepad_motion_t &motion) {
    // Unimplemented
  }

  /**
   * @brief Sends a gamepad battery event to the OS.
   * @param input The global input context.
   * @param battery The battery event.
   */
  void gamepad_battery(input_t &input, const gamepad_battery_t &battery) {
    // Unimplemented
  }

  input_t input() {
    input_t result {new macos_input_t()};

    const auto macos_input = static_cast<macos_input_t *>(result.get());

    // Default to main display
    macos_input->display = CGMainDisplayID();

    auto output_name = display_device::map_output_name(config::video.output_name);
    // If output_name is set, try to find the display with that display id
    if (!output_name.empty()) {
      uint32_t max_display = 32;
      uint32_t display_count;
      CGDirectDisplayID displays[max_display];
      if (CGGetActiveDisplayList(max_display, displays, &display_count) != kCGErrorSuccess) {
        BOOST_LOG(error) << "Unable to get active display list , error: "sv << std::endl;
      } else {
        for (int i = 0; i < display_count; i++) {
          CGDirectDisplayID display_id = displays[i];
          if (display_id == std::atoi(output_name.c_str())) {
            macos_input->display = display_id;
          }
        }
      }
    }

    // Input coordinates are based on the virtual resolution not the physical, so we need the scaling factor
    const CGDisplayModeRef mode = CGDisplayCopyDisplayMode(macos_input->display);
    macos_input->displayScaling = ((CGFloat) CGDisplayPixelsWide(macos_input->display)) / ((CGFloat) CGDisplayModeGetPixelWidth(mode));
    CFRelease(mode);

    macos_input->source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);

    macos_input->kb_event = CGEventCreate(macos_input->source);
    macos_input->kb_flags = 0;

    macos_input->mouse_event = CGEventCreate(macos_input->source);
    macos_input->mouse_down[0] = false;
    macos_input->mouse_down[1] = false;
    macos_input->mouse_down[2] = false;

    BOOST_LOG(debug) << "Display "sv << macos_input->display << ", pixel dimension: " << CGDisplayPixelsWide(macos_input->display) << "x"sv << CGDisplayPixelsHigh(macos_input->display);

    return result;
  }

  void freeInput(void *p) {
    const auto *input = static_cast<macos_input_t *>(p);

    CFRelease(input->source);
    CFRelease(input->kb_event);
    CFRelease(input->mouse_event);

    delete input;
  }

  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input) {
    if (!input) {
      static std::vector defaults {
        supported_gamepad_t {"auto", true, ""},
        supported_gamepad_t {"hid", true, ""}
      };
      return defaults;
    }

    auto macos_input = static_cast<macos_input_t *>(input);
    const bool supported = ensure_virtual_gamepad_support(*macos_input);
    static std::vector availability {
      supported_gamepad_t {"auto", true, ""},
      supported_gamepad_t {"hid", false, "gamepads.macos_hid_unavailable"}
    };

    for (auto &entry : availability) {
      if (entry.name == "hid") {
        entry.is_enabled = supported;
        entry.reason_disabled = supported ? "" : "gamepads.macos_hid_unavailable";
      }
    }

    if (!supported) {
      BOOST_LOG(warning) << "macOS virtual gamepad backend unavailable"sv;
    }

    return availability;
  }

  /**
   * @brief Returns the supported platform capabilities to advertise to the client.
   * @return Capability flags.
   */
  platform_caps::caps_t get_capabilities() {
    return 0;
  }
}  // namespace platf
