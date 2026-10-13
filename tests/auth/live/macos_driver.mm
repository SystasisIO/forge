#include "common.hxx"

#import <AppKit/AppKit.h>
#include <dispatch/dispatch.h>

int main(int argc, char** argv) {
   @autoreleasepool {
      [NSApplication sharedApplication];
      [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
      // Named Forge modules are consumed by appauth_driver.cpp, not by an ObjC++ BMI.
      return auth_live::run_appauth(argc, argv, [] { [NSApp run]; }, [] {
         dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp stop:nil];
            [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                               location:NSZeroPoint modifierFlags:0 timestamp:0
                                           windowNumber:0 context:nil subtype:0 data1:0 data2:0]
                     atStart:YES];
         });
      });
   }
}
