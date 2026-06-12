/* fn_key.m --- translate the macOS Fn/Globe modifier to Option for Electron.

   The user remaps the physical Option key to Globe/Fn at the macOS
   system level.  Native Emacs can see NSEventModifierFlagFunction
   (`ns-function-modifier'), but browsers/Chromium cannot -- the
   Function flag is stripped long before web content sees the event.

   This module installs an NSEvent local monitor in the Electron main
   process that rewrites key events BEFORE Chromium processes them:
   if the Function flag is set (and the key is not a real function/
   navigation key, which carry that flag naturally), replace it with
   Option.  The web client's input.js then maps Option per its normal
   modifier table.

   Usage from main.js:
     const fnKey = require('./build/Release/fn_key.node');
     fnKey.start();   // install monitor
     fnKey.stop();    // remove monitor
*/

#import <AppKit/AppKit.h>
#include <node_api.h>

static id fn_monitor = nil;

/* Keys that legitimately carry the Function modifier flag: F-keys,
   the navigation cluster, arrows, and the Fn key itself.  Translating
   these would turn PageDown into M-PageDown etc.  */
static bool
fn_is_function_key (unsigned short keycode)
{
  switch (keycode)
    {
    case 63:                                    /* Fn itself */
    case 96: case 97: case 98: case 99: case 100: case 101: /* F5-F9... */
    case 103: case 105: case 106: case 107: case 109: case 111:
    case 113: case 114: case 118: case 120: case 122:        /* F-keys */
    case 64: case 79: case 80: case 90:                      /* F17-F20 */
    case 115: case 116: case 117: case 119: case 121:        /* nav cluster */
    case 123: case 124: case 125: case 126:                  /* arrows */
      return true;
    default:
      return false;
    }
}

static NSEvent *
fn_translate (NSEvent *event)
{
  NSEventModifierFlags flags = event.modifierFlags;
  if (!(flags & NSEventModifierFlagFunction))
    return event;
  if (event.type != NSEventTypeFlagsChanged
      && fn_is_function_key (event.keyCode))
    return event;

  NSEventModifierFlags newFlags =
    (flags & ~NSEventModifierFlagFunction) | NSEventModifierFlagOption;

  if (event.type == NSEventTypeFlagsChanged)
    return [NSEvent keyEventWithType:NSEventTypeFlagsChanged
                            location:event.locationInWindow
                       modifierFlags:newFlags
                           timestamp:event.timestamp
                        windowNumber:event.windowNumber
                             context:nil
                          characters:@""
         charactersIgnoringModifiers:@""
                           isARepeat:NO
                             keyCode:event.keyCode];

  /* Use the unmodified characters: the web client derives the
     intended character from the physical key code when Option is
     held (codeToChar in input.js), so composed characters from the
     OS layer are unwanted here.  */
  NSString *chars = event.charactersIgnoringModifiers ?: @"";
  return [NSEvent keyEventWithType:event.type
                          location:event.locationInWindow
                     modifierFlags:newFlags
                         timestamp:event.timestamp
                      windowNumber:event.windowNumber
                           context:nil
                        characters:chars
       charactersIgnoringModifiers:chars
                         isARepeat:event.isARepeat
                           keyCode:event.keyCode];
}

static napi_value
fn_start (napi_env env, napi_callback_info info)
{
  dispatch_async (dispatch_get_main_queue (), ^{
    if (fn_monitor != nil)
      return;
    fn_monitor =
      [NSEvent addLocalMonitorForEventsMatchingMask:
                 (NSEventMaskKeyDown | NSEventMaskKeyUp
                  | NSEventMaskFlagsChanged)
                                            handler:^NSEvent * (NSEvent *e) {
        return fn_translate (e);
      }];
  });
  return NULL;
}

static napi_value
fn_stop (napi_env env, napi_callback_info info)
{
  dispatch_async (dispatch_get_main_queue (), ^{
    if (fn_monitor != nil)
      {
        [NSEvent removeMonitor:fn_monitor];
        fn_monitor = nil;
      }
  });
  return NULL;
}

static napi_value
fn_key_init (napi_env env, napi_value exports)
{
  napi_value start_fn, stop_fn;
  napi_create_function (env, "start", NAPI_AUTO_LENGTH, fn_start, NULL,
                        &start_fn);
  napi_create_function (env, "stop", NAPI_AUTO_LENGTH, fn_stop, NULL,
                        &stop_fn);
  napi_set_named_property (env, exports, "start", start_fn);
  napi_set_named_property (env, exports, "stop", stop_fn);
  return exports;
}

NAPI_MODULE (fn_key, fn_key_init)
