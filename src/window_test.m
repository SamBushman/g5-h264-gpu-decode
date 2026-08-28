/*
 * window-test: Milestone 9, minimal on-screen presentation smoke test.
 * Deliberately as simple as possible - a bare NSWindow + NSOpenGLView,
 * no text fields/docks/drag-capable controls (the kind of AppKit control
 * that actually calls registerDragTypes: and is what the documented
 * CoreDrag/SSH deadlock quirk was originally triggered by) - to keep
 * this test's own risk as low as it can be while still using a real
 * window. Cycles the clear color for ~30s so "is it alive" is
 * unambiguous to someone watching, then exits cleanly on its own.
 */

#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>

int main(int argc, char *argv[]) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];

    NSRect frame = NSMakeRect(100, 100, 640, 360);
    NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:(NSTitledWindowMask | NSClosableWindowMask | NSMiniaturizableWindowMask)
        backing:NSBackingStoreBuffered defer:NO];
    [window setTitle:@"X1900 GPU Decode - M9 window test"];

    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 24, 0
    };
    NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    NSOpenGLView *glView = [[NSOpenGLView alloc] initWithFrame:[[window contentView] bounds] pixelFormat:pf];
    [window setContentView:glView];
    [[glView openGLContext] makeCurrentContext];

    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    fprintf(stderr, "window shown, starting color cycle\n");
    fflush(stderr);

    for (int i = 0; i < 60; i++) {
        float t = (i % 20) / 20.0f;
        glClearColor(t, 0.2f, 1.0f - t, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        [[glView openGLContext] flushBuffer];
        fprintf(stderr, "frame %d\n", i);
        fflush(stderr);
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
    }

    fprintf(stderr, "done, exiting cleanly\n");
    [pool release];
    return 0;
}
