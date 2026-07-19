#import <Cocoa/Cocoa.h>
#include "MacWindow.h"
#include <mutex>
#include <vector>

@interface MacPreviewView : NSView {
    std::mutex _mutex;
    std::vector<uint8_t> _buffer;
    int _width;
    int _height;
}
- (void)updateBuffer:(const void*)buffer width:(int)width height:(int)height;
@end

@implementation MacPreviewView

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)keyDown:(NSEvent *)event {
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] > 0) {
        unichar code = [chars characterAtIndex:0];
        if (code == NSF11FunctionKey) {
            [[self window] toggleFullScreen:nil];
            return;
        }
    }
    [super keyDown:event];
}

- (void)updateBuffer:(const void*)buffer width:(int)width height:(int)height {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        size_t size = width * height * 4;
        if (_buffer.size() != size) {
            _buffer.resize(size);
        }
        memcpy(_buffer.data(), buffer, size);
        _width = width;
        _height = height;
    }
    
    // Always render on main queue async
    dispatch_async(dispatch_get_main_queue(), ^{
        [self setNeedsDisplay:YES];
    });
}

- (void)drawRect:(NSRect)dirtyRect {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_buffer.empty() || _width <= 0 || _height <= 0) {
        [[NSColor blackColor] setFill];
        NSRectFill(dirtyRect);
        return;
    }

    CGContextRef context = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
    if (!context) return;

    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, _buffer.data(), _buffer.size(), NULL);
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    
    CGImageRef image = CGImageCreate(
        _width, _height,
        8, 32, _width * 4,
        colorSpace,
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Big,
        provider,
        NULL,
        NO,
        kCGRenderingIntentDefault
    );

    if (image) {
        NSRect bounds = [self bounds];
        CGRect rect = CGRectMake(bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height);
        
        CGContextDrawImage(context, rect, image);
        
        CGImageRelease(image);
    }

    CGColorSpaceRelease(colorSpace);
    CGDataProviderRelease(provider);
}

@end

@interface MacWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation MacWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    return YES;
}
@end

static MacWindowDelegate* g_windowDelegate = nil;

extern "C" {

MacWindowHandle CreateMacWindow(const std::string& title, int width, int height) {
    __block MacWindowHandle handle = nil;
    
    void (^block)(void) = ^{
        // Initialize NSApplication
        [NSApplication sharedApplication];
        if ([NSApp activationPolicy] == NSApplicationActivationPolicyProhibited) {
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            [NSApp finishLaunching];
        }
        
        NSRect frame = NSMakeRect(100, 100, width, height);
        NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                     styleMask:(NSWindowStyleMaskTitled | 
                                                                NSWindowStyleMaskClosable | 
                                                                NSWindowStyleMaskResizable |
                                                                NSWindowStyleMaskMiniaturizable)
                                                       backing:NSBackingStoreBuffered
                                                         defer:NO];
        [window setTitle:[NSString stringWithUTF8String:title.c_str()]];
        
        MacPreviewView* view = [[MacPreviewView alloc] initWithFrame:frame];
        [window setContentView:view];
        [window setReleasedWhenClosed:NO];
        
        if (!g_windowDelegate) {
            g_windowDelegate = [[MacWindowDelegate alloc] init];
        }
        [window setDelegate:g_windowDelegate];
        [window makeFirstResponder:view];
        
        handle = (__bridge_retained void*)window;
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }
    
    return handle;
}

void ShowMacWindow(MacWindowHandle window, bool show) {
    if (!window) return;
    NSWindow* nsWindow = (__bridge NSWindow*)window;
    
    void (^block)(void) = ^{
        if (show) {
            [nsWindow makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
        } else {
            [nsWindow orderOut:nil];
        }
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

void BlitToMacWindow(MacWindowHandle window, const void* buffer, int width, int height) {
    if (!window || !buffer) return;
    NSWindow* nsWindow = (__bridge NSWindow*)window;
    MacPreviewView* view = (MacPreviewView*)[nsWindow contentView];
    if ([view isKindOfClass:[MacPreviewView class]]) {
        [view updateBuffer:buffer width:width height:height];
    }
}

void DestroyMacWindow(MacWindowHandle window) {
    if (!window) return;
    
    void (^block)(void) = ^{
        NSWindow* nsWindow = (__bridge_transfer NSWindow*)window;
        [nsWindow setDelegate:nil];
        [nsWindow close];
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }
}

void ProcessMacWindowEvents() {
    @autoreleasepool {
        NSEvent* event;
        do {
            event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES];
            if (event) {
                [NSApp sendEvent:event];
            }
        } while (event);
    }
}

} // extern "C"
