/***********************************************************************
 * hjp_platform_macos.m — はじむGUI macOS プラットフォーム実装
 *
 * SDL2 の完全ドロップイン代替。
 * Cocoa + CoreText + ImageIO + OpenGL を使用。
 *
 * Copyright (c) 2026 Reo Shiozawa — MIT License
 ***********************************************************************/

#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <OpenGL/gl3.h>
#import <mach/mach_time.h>

// CocoaオブジェクトはARCで管理する。MRCビルドでの描画ごとのリークを防ぐ。
#if !__has_feature(objc_arc)
#error "hjp_platform_macos.m requires -fobjc-arc"
#endif

#include "hjp_platform.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* =====================================================================
 * 内部型
 * ===================================================================*/

@class HjpNSWindow;
@class HjpOpenGLView;
@class HjpAppDelegate;
@class HjpWindowDelegate;

#define HJP_MAX_EVENTS 256
#define HJP_MAX_KEYS   512

struct HjpWindow {
    HjpNSWindow  *nswin;
    HjpOpenGLView *glview;
    NSOpenGLContext *glctx;
    HjpWindowDelegate *delegate;
    int windowId;
    bool closed;
};

/* グローバル */
static struct {
    bool initialized;
    HjpAppDelegate *appDelegate;
    int nextWindowId;
    mach_timebase_info_data_t timebase;
    uint64_t startTime;

    /* イベントキュー (リングバッファ) */
    HjpEvent events[HJP_MAX_EVENTS];
    int evHead, evTail;

    /* キーボード状態 */
    uint8_t keys[HJP_MAX_KEYS];

    /* マウス状態 */
    int mouseX, mouseY;
    uint32_t mouseButtons;

    HjpWindow *mainWindow;
} g_hjp = {0};

static void hjp__pushEvent(HjpEvent *e) {
    int next = (g_hjp.evHead + 1) % HJP_MAX_EVENTS;
    if (next == g_hjp.evTail) return; /* full */
    g_hjp.events[g_hjp.evHead] = *e;
    g_hjp.evHead = next;
}

static bool hjp__popEvent(HjpEvent *e) {
    if (g_hjp.evHead == g_hjp.evTail) return false;
    *e = g_hjp.events[g_hjp.evTail];
    g_hjp.evTail = (g_hjp.evTail + 1) % HJP_MAX_EVENTS;
    return true;
}

/* =====================================================================
 * ネイティブメニューバー
 * ===================================================================*/
@interface HjpMenuTarget : NSObject
- (void)menuAction:(id)sender;
@end

static HjpMenuTarget *g_menuTarget = nil;
static volatile uint32_t g_menuClickedTag = 0;
static NSMenu *g_buildingSubMenu = nil;

@implementation HjpMenuTarget
- (void)menuAction:(id)sender {
    NSMenuItem *item = (NSMenuItem *)sender;
    g_menuClickedTag = (uint32_t)item.tag;
}
@end

void hjp_native_menubar_init(void) {
    @autoreleasepool {
        if (!g_menuTarget) g_menuTarget = [[HjpMenuTarget alloc] init];
        NSMenu *mainMenu = [NSApp mainMenu];
        /* Keep app menu (index 0), remove rest */
        while ([mainMenu numberOfItems] > 1) {
            [mainMenu removeItemAtIndex:1];
        }
        g_buildingSubMenu = nil;
    }
}

void hjp_native_menubar_begin_menu(const char *title) {
    @autoreleasepool {
        NSString *t = [NSString stringWithUTF8String:title];
        NSMenuItem *menuItem = [[NSMenuItem alloc] initWithTitle:t
                                                         action:nil
                                                  keyEquivalent:@""];
        g_buildingSubMenu = [[NSMenu alloc] initWithTitle:t];
        g_buildingSubMenu.autoenablesItems = NO;
        [menuItem setSubmenu:g_buildingSubMenu];
        [[NSApp mainMenu] addItem:menuItem];
    }
}

static void hjp__enable_menu_item(NSMenu *menu, uint32_t tag, bool enabled) {
    for (NSMenuItem *item in menu.itemArray) {
        if ((uint32_t)item.tag == tag && item.action == @selector(menuAction:)) item.enabled = enabled;
        if (item.submenu) hjp__enable_menu_item(item.submenu, tag, enabled);
    }
}
void hjp_native_menubar_enable_item(uint32_t tag, bool enabled) {
    hjp__enable_menu_item(NSApp.mainMenu, tag, enabled);
}

void hjp_native_menubar_add_item(const char *title, const char *shortcut, uint32_t tag) {
    if (!g_buildingSubMenu) return;
    @autoreleasepool {
        NSString *t = [NSString stringWithUTF8String:title];
        NSString *key = @"";
        NSUInteger mask = 0;

        if (shortcut && *shortcut) {
            const char *rest = shortcut;
            /* Parse modifiers: Ctrl+/Cmd+/Shift+/Alt+ */
            while (1) {
                if (strncmp(rest, "Ctrl+", 5) == 0) {
                    mask |= NSEventModifierFlagCommand; rest += 5;
                } else if (strncmp(rest, "Cmd+", 4) == 0) {
                    mask |= NSEventModifierFlagCommand; rest += 4;
                } else if (strncmp(rest, "Shift+", 6) == 0) {
                    mask |= NSEventModifierFlagShift; rest += 6;
                } else if (strncmp(rest, "Alt+", 4) == 0) {
                    mask |= NSEventModifierFlagOption; rest += 4;
                } else break;
            }
            /* Parse key */
            if (rest[0] == 'F' && rest[1] >= '1' && rest[1] <= '9') {
                int fn = atoi(rest + 1);
                if (fn >= 1 && fn <= 15) {
                    unichar fkey = NSF1FunctionKey + (fn - 1);
                    key = [NSString stringWithCharacters:&fkey length:1];
                    /* Function keys: modifier なし (system items only) */
                    mask = 0;
                }
            } else if (*rest) {
                char lower = tolower(*rest);
                key = [NSString stringWithFormat:@"%c", lower];
            }
        }

        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:t
                                                      action:@selector(menuAction:)
                                               keyEquivalent:key];
        [item setKeyEquivalentModifierMask:mask];
        [item setTarget:g_menuTarget];
        [item setTag:(NSInteger)tag];
        [g_buildingSubMenu addItem:item];
    }
}

void hjp_native_menubar_add_separator(void) {
    if (!g_buildingSubMenu) return;
    @autoreleasepool {
        [g_buildingSubMenu addItem:[NSMenuItem separatorItem]];
    }
}

void hjp_native_menubar_end_menu(void) {
    g_buildingSubMenu = nil;
}

uint32_t hjp_native_menubar_poll_clicked(void) {
    uint32_t tag = g_menuClickedTag;
    g_menuClickedTag = 0;
    return tag;
}

/* =====================================================================
 * キーコード変換
 * ===================================================================*/
static HjpKeycode hjp__macKeyToHjp(unsigned short keyCode) {
    switch (keyCode) {
    case 0x00: return HJPK_a;
    case 0x08: return HJPK_c;
    case 0x09: return HJPK_v;
    case 0x07: return HJPK_x;
    case 0x24: return HJPK_RETURN;
    case 0x4C: return HJPK_KP_ENTER;
    case 0x35: return HJPK_ESCAPE;
    case 0x33: return HJPK_BACKSPACE;
    case 0x30: return HJPK_TAB;
    case 0x75: return HJPK_DELETE;
    case 0x7B: return HJPK_LEFT;
    case 0x7C: return HJPK_RIGHT;
    case 0x7D: return HJPK_DOWN;
    case 0x7E: return HJPK_UP;
    case 0x73: return HJPK_HOME;
    case 0x77: return HJPK_END;
    default: break;
    }
    /* ASCII 文字 — characters を使う (呼び出し側で設定) */
    return HJPK_UNKNOWN;
}

static HjpKeymod hjp__macModToHjp(NSUInteger modFlags) {
    HjpKeymod mod = HJP_KMOD_NONE;
    if (modFlags & NSEventModifierFlagShift)   mod |= HJP_KMOD_SHIFT;
    if (modFlags & NSEventModifierFlagControl) mod |= HJP_KMOD_CTRL;
    if (modFlags & NSEventModifierFlagOption)  mod |= HJP_KMOD_ALT;
    if (modFlags & NSEventModifierFlagCommand) mod |= HJP_KMOD_GUI;
    return mod;
}

static int hjp__macKeyToScancode(unsigned short keyCode) {
    switch (keyCode) {
    case 0x24: return HJP_SCANCODE_RETURN;
    case 0x35: return HJP_SCANCODE_ESCAPE;
    case 0x33: return HJP_SCANCODE_BACKSPACE;
    case 0x30: return HJP_SCANCODE_TAB;
    case 0x7E: return HJP_SCANCODE_UP;
    case 0x7D: return HJP_SCANCODE_DOWN;
    case 0x7B: return HJP_SCANCODE_LEFT;
    case 0x7C: return HJP_SCANCODE_RIGHT;
    default: return -1;
    }
}

/* =====================================================================
 * NSOpenGLView サブクラス
 * ===================================================================*/
@class HjpAccessibilityElement;
@interface HjpOpenGLView : NSOpenGLView <NSTextInputClient>
{
    NSTrackingArea *trackingArea;
}
@property(nonatomic, copy) NSString *composition;
@property(nonatomic) NSRange compositionSelection;
@property(nonatomic, copy) NSString *inputDocument;
@property(nonatomic) NSRange documentSelection;
@property(nonatomic) NSRange compositionBase;
@property(nonatomic) uint32_t documentID;
@property(nonatomic) int documentCapacity;
@property(nonatomic) NSUInteger documentAnchor;
@property(nonatomic) NSUInteger documentCursor;
@property(nonatomic) BOOL handledTextCommand;
- (void)clearComposition;
@property(nonatomic) NSRect inputRect;
@property(nonatomic, assign) HjpWindow *ownerWindow;
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, HjpAccessibilityElement *> *accessibleCache;
@property(nonatomic, strong) NSMutableArray<HjpAccessibilityElement *> *accessibleFrame;
@property(nonatomic, copy) NSArray<HjpAccessibilityElement *> *accessiblePublished;
@end

@interface HjpAccessibilityElement : NSAccessibilityElement
@property(nonatomic, weak) HjpOpenGLView *ownerView;
@property(nonatomic) uint32_t widgetID;
@property(nonatomic) BOOL live;
@end
@implementation HjpAccessibilityElement
- (id)accessibilityParent { return self.ownerView; }
- (NSRect)accessibilityFrame {
    HjpOpenGLView *view = self.ownerView;
    if (!view.window) return NSZeroRect;
    return [view.window convertRectToScreen:[view convertRect:self.accessibilityFrameInParentSpace toView:nil]];
}
- (BOOL)accessibilityPerformPress {
    HjpOpenGLView *view = self.ownerView;
    if (!self.live || !self.accessibilityEnabled || !view.ownerWindow) return NO;
    HjpEvent e = {0};
    e.type = HJP_EVENT_ACCESSIBILITY_PRESS;
    e.accessibility.window = view.ownerWindow;
    e.accessibility.id = self.widgetID;
    e.accessibility.generation = view.ownerWindow->windowId;
    hjp__pushEvent(&e);
    return YES;
}
@end

@implementation HjpOpenGLView

- (BOOL)isAccessibilityElement { return YES; }
- (NSString *)accessibilityRole { return NSAccessibilityGroupRole; }
- (NSArray *)accessibilityChildren { return self.accessiblePublished ?: @[]; }
- (id)accessibilityHitTest:(NSPoint)point {
    for (HjpAccessibilityElement *element in [self.accessiblePublished reverseObjectEnumerator])
        if (element.live && NSPointInRect(point, element.accessibilityFrame)) return element;
    return self;
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return YES; }

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (trackingArea) [self removeTrackingArea:trackingArea];
    trackingArea = [[NSTrackingArea alloc]
        initWithRect:self.bounds
        options:(NSTrackingMouseMoved|NSTrackingActiveInKeyWindow|NSTrackingInVisibleRect)
        owner:self userInfo:nil];
    [self addTrackingArea:trackingArea];
}

/* --- マウスイベント --- */
- (void)mouseMoved:(NSEvent *)event { [self handleMouseMove:event]; }
- (void)mouseDragged:(NSEvent *)event { [self handleMouseMove:event]; }
- (void)rightMouseDragged:(NSEvent *)event { [self handleMouseMove:event]; }
- (void)otherMouseDragged:(NSEvent *)event { [self handleMouseMove:event]; }

- (void)handleMouseMove:(NSEvent *)event {
    NSPoint loc = [self convertPoint:[event locationInWindow] fromView:nil];
    loc.y = self.bounds.size.height - loc.y; /* flip Y */
    g_hjp.mouseX = (int)loc.x;
    g_hjp.mouseY = (int)loc.y;
    HjpEvent e = {0};
    e.type = HJP_EVENT_MOUSEMOTION;
    e.motion.x = (int)loc.x;
    e.motion.y = (int)loc.y;
    hjp__pushEvent(&e);
}

- (void)mouseDown:(NSEvent *)event { [self handleMouseBtn:event btn:HJP_BUTTON_LEFT down:YES]; }
- (void)mouseUp:(NSEvent *)event   { [self handleMouseBtn:event btn:HJP_BUTTON_LEFT down:NO]; }
- (void)rightMouseDown:(NSEvent *)event { [self handleMouseBtn:event btn:HJP_BUTTON_RIGHT down:YES]; }
- (void)rightMouseUp:(NSEvent *)event   { [self handleMouseBtn:event btn:HJP_BUTTON_RIGHT down:NO]; }
- (void)otherMouseDown:(NSEvent *)event { [self handleMouseBtn:event btn:HJP_BUTTON_MIDDLE down:YES]; }
- (void)otherMouseUp:(NSEvent *)event   { [self handleMouseBtn:event btn:HJP_BUTTON_MIDDLE down:NO]; }

- (void)handleMouseBtn:(NSEvent *)event btn:(int)btn down:(BOOL)down {
    NSPoint loc = [self convertPoint:[event locationInWindow] fromView:nil];
    loc.y = self.bounds.size.height - loc.y;
    HjpEvent e = {0};
    e.type = down ? HJP_EVENT_MOUSEBUTTONDOWN : HJP_EVENT_MOUSEBUTTONUP;
    e.button.button = btn;
    e.button.mod = hjp__macModToHjp(event.modifierFlags);
    e.button.x = (int)loc.x;
    e.button.y = (int)loc.y;
    hjp__pushEvent(&e);
    if (down) g_hjp.mouseButtons |= HJP_BUTTON(btn);
    else g_hjp.mouseButtons &= ~HJP_BUTTON(btn);
}

- (void)scrollWheel:(NSEvent *)event {
    HjpEvent e = {0};
    e.type = HJP_EVENT_MOUSEWHEEL;
    e.wheel.x = (int)[event scrollingDeltaX];
    e.wheel.y = (int)[event scrollingDeltaY];
    /* Normal scroll → discrete */
    if (![event hasPreciseScrollingDeltas]) {
        e.wheel.x = (int)[event deltaX];
        e.wheel.y = (int)[event deltaY];
    }
    hjp__pushEvent(&e);
}

/* --- キーイベント --- */
- (void)keyDown:(NSEvent *)event {
    HjpKeycode sym = hjp__macKeyToHjp([event keyCode]);
    if (sym == HJPK_UNKNOWN && [[event characters] length] > 0) {
        unichar ch = [[event characters] characterAtIndex:0];
        if (ch < 128) sym = (HjpKeycode)ch;
    }
    int sc = hjp__macKeyToScancode([event keyCode]);
    if (sc >= 0 && sc < HJP_MAX_KEYS) g_hjp.keys[sc] = 1;

    HjpEvent e = {0};
    e.type = HJP_EVENT_KEYDOWN;
    e.key.sym = sym;
    e.key.mod = hjp__macModToHjp([event modifierFlags]);
    BOOL wasMarked = self.hasMarkedText;
    self.handledTextCommand = NO;
    /* 確定本文を持つ場合、標準の編集操作をOS側の文書状態へ先に反映する。 */
    if (self.documentID && !wasMarked && (event.modifierFlags & NSEventModifierFlagCommand)) {
        if (sym == HJPK_a) {
            [self doCommandBySelector:@selector(selectAll:)];
        } else if (sym == HJPK_c || sym == HJPK_x) {
            NSString *selected = [self.inputDocument substringWithRange:self.documentSelection];
            if (selected.length) {
                hjp_set_clipboard_text(selected.UTF8String);
                if (sym == HJPK_x) [self insertText:@"" replacementRange:self.documentSelection];
            }
            self.handledTextCommand = YES;
        } else if (sym == HJPK_v) {
            char *clip = hjp_get_clipboard_text();
            if (clip) {
                NSString *pasted = [[NSString alloc] initWithUTF8String:clip];
                if (pasted.length) [self insertText:pasted replacementRange:NSMakeRange(NSNotFound, 0)];
                hjp_free(clip);
            }
            self.handledTextCommand = YES;
        }
    }
    /* テキスト入力 (interpretKeyEvents を通す)。変換中の操作を二重配送しない。 */
    if (!self.handledTextCommand) [self interpretKeyEvents:@[event]];
    if (!wasMarked && !self.handledTextCommand) hjp__pushEvent(&e);
}
- (void)keyUp:(NSEvent *)event {
    HjpKeycode sym = hjp__macKeyToHjp([event keyCode]);
    if (sym == HJPK_UNKNOWN && [[event characters] length] > 0) {
        unichar ch = [[event characters] characterAtIndex:0];
        if (ch < 128) sym = (HjpKeycode)ch;
    }
    int sc = hjp__macKeyToScancode([event keyCode]);
    if (sc >= 0 && sc < HJP_MAX_KEYS) g_hjp.keys[sc] = 0;

    HjpEvent e = {0};
    e.type = HJP_EVENT_KEYUP;
    e.key.sym = sym;
    e.key.mod = hjp__macModToHjp([event modifierFlags]);
    hjp__pushEvent(&e);
}

/* --- NSTextInputClient --- */
- (NSString *)documentWithComposition {
    NSString *document = self.inputDocument ?: @"";
    if (![self hasMarkedText]) return document;
    return [document stringByReplacingCharactersInRange:self.compositionBase withString:self.composition];
}

/* OSの仮想文書上の範囲を、未確定文字を除く確定本文へ戻す。 */
- (NSRange)committedRange:(NSRange)range {
    NSString *document = self.inputDocument ?: @"";
    if (range.location == NSNotFound)
        return [self hasMarkedText] ? self.compositionBase : self.documentSelection;
    NSUInteger total = [self documentWithComposition].length;
    if (range.location > total || range.length > total - range.location) return NSMakeRange(NSNotFound, 0);
    if (![self hasMarkedText]) return range;
    NSUInteger a = self.compositionBase.location, b = a + self.composition.length;
    NSUInteger start = range.location, end = NSMaxRange(range);
    start = start <= a ? start : start >= b ? start - self.composition.length + self.compositionBase.length : a;
    end = end <= a ? end : end >= b ? end - self.composition.length + self.compositionBase.length : NSMaxRange(self.compositionBase);
    if (start > document.length || end > document.length || start > end) return NSMakeRange(NSNotFound, 0);
    return NSMakeRange(start, end - start);
}

- (BOOL)emitDocumentEdit:(NSRange)range text:(NSString *)text selectionOnly:(BOOL)selectionOnly {
    NSString *document = self.inputDocument ?: @"";
    if (!self.ownerWindow || !self.documentID || range.location == NSNotFound ||
        range.location > document.length || range.length > document.length - range.location) return NO;
    NSData *prefix = [[document substringToIndex:range.location] dataUsingEncoding:NSUTF8StringEncoding];
    NSData *removed = [[document substringWithRange:range] dataUsingEncoding:NSUTF8StringEncoding];
    if (!prefix || !removed) return NO; /* UTF-16サロゲートの途中を拒否する。 */
    const char *utf8 = text.UTF8String;
    if (!utf8) return NO;
    size_t remaining = strlen(utf8), offset = 0;
    size_t required = 1;
    if (!selectionOnly && remaining) {
        required = 0;
        for (size_t position = 0; position < remaining; required++) {
            size_t n = MIN(remaining - position, (size_t)127);
            while (n && ((unsigned char)utf8[position + n] & 0xc0) == 0x80) n--;
            position += n;
        }
    }
    int freeEvents = HJP_MAX_EVENTS - 1 -
        (g_hjp.evHead - g_hjp.evTail + HJP_MAX_EVENTS) % HJP_MAX_EVENTS;
    /* 後続の未確定状態通知も含め、置換の一部だけを配送しない。 */
    if (required + 1 > (size_t)freeEvents) return NO;
    do {
        HjpEvent event = {0};
        event.type = selectionOnly ? HJP_EVENT_TEXTSELECTION : HJP_EVENT_TEXTREPLACE;
        event.edit.window = self.ownerWindow;
        event.edit.generation = self.ownerWindow->windowId;
        event.edit.id = self.documentID;
        event.edit.start = (int)(prefix.length + offset);
        event.edit.end = event.edit.start + (offset ? 0 : (int)removed.length);
        event.edit.cursor = event.edit.end;
        size_t n = MIN(remaining, sizeof(event.edit.text) - 1);
        while (n && ((unsigned char)utf8[offset + n] & 0xc0) == 0x80) n--;
        if (n) memcpy(event.edit.text, utf8 + offset, n);
        hjp__pushEvent(&event);
        offset += n;
        remaining -= n;
        if (selectionOnly) break;
    } while (remaining);
    return YES;
}

- (void)insertText:(id)aString replacementRange:(NSRange)replacementRange {
    NSString *str = ([aString isKindOfClass:[NSAttributedString class]])
                    ? [aString string] : aString;
    if (self.documentID) {
        NSRange range = [self committedRange:replacementRange];
        NSString *document = self.inputDocument ?: @"";
        if (range.location == NSNotFound) return;
        NSUInteger retained = [document lengthOfBytesUsingEncoding:NSUTF8StringEncoding] -
            [[document substringWithRange:range] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
        NSUInteger available = (NSUInteger)MAX(0, self.documentCapacity - 1) > retained ? self.documentCapacity - 1 - retained : 0;
        const char *bytes = str.UTF8String;
        if (!bytes) return;
        size_t n = MIN(strlen(bytes), available);
        while (n && ((unsigned char)bytes[n] & 0xc0) == 0x80) n--;
        if (str.length && !n) { [self clearComposition]; return; }
        str = [[NSString alloc] initWithBytes:bytes length:n encoding:NSUTF8StringEncoding];
        if (![self emitDocumentEdit:range text:str selectionOnly:NO]) return;
        self.inputDocument = [document stringByReplacingCharactersInRange:range withString:str];
        self.documentSelection = NSMakeRange(range.location + str.length, 0);
        self.documentAnchor = self.documentCursor = self.documentSelection.location;
        [self clearComposition];
        return;
    }
    if ([self hasMarkedText]) [self clearComposition];
    const char *utf8 = [str UTF8String];
    /* キューの1イベントに収まるUTF-8境界で分割する。 */
    while (utf8 && *utf8) {
        HjpEvent e = {0};
        e.type = HJP_EVENT_TEXTINPUT;
        size_t n = strlen(utf8);
        if (n >= sizeof(e.text.text)) {
            n = sizeof(e.text.text) - 1;
            while (n && ((unsigned char)utf8[n] & 0xc0) == 0x80) n--;
        }
        memcpy(e.text.text, utf8, n);
        hjp__pushEvent(&e);
        utf8 += n;
    }
}
- (void)doCommandBySelector:(SEL)aSelector {
    if (!self.documentID || self.hasMarkedText) return;
    NSString *name = NSStringFromSelector(aSelector);
    BOOL extend = [name hasSuffix:@"AndModifySelection:"];
    BOOL left = [name isEqualToString:@"moveLeft:"] || [name isEqualToString:@"moveLeftAndModifySelection:"];
    BOOL right = [name isEqualToString:@"moveRight:"] || [name isEqualToString:@"moveRightAndModifySelection:"];
    BOOL home = [name isEqualToString:@"moveToBeginningOfDocument:"] || [name isEqualToString:@"moveToBeginningOfDocumentAndModifySelection:"];
    BOOL end = [name isEqualToString:@"moveToEndOfDocument:"] || [name isEqualToString:@"moveToEndOfDocumentAndModifySelection:"];
    BOOL all = [name isEqualToString:@"selectAll:"];
    if ([name isEqualToString:@"deleteBackward:"] || [name isEqualToString:@"deleteForward:"]) {
        NSRange range = self.documentSelection;
        if (!range.length) {
            if ([name isEqualToString:@"deleteBackward:"] && range.location)
                range = [self.inputDocument rangeOfComposedCharacterSequenceAtIndex:range.location - 1];
            else if ([name isEqualToString:@"deleteForward:"] && range.location < self.inputDocument.length)
                range = [self.inputDocument rangeOfComposedCharacterSequenceAtIndex:range.location];
        }
        [self insertText:@"" replacementRange:range];
        self.handledTextCommand = YES;
        return;
    }
    if (left || right || home || end || all) {
        NSUInteger cursor = self.documentCursor, anchor = self.documentAnchor;
        if (all) { anchor = 0; cursor = self.inputDocument.length; }
        else if (left) cursor = !extend && self.documentSelection.length ? self.documentSelection.location :
            cursor ? [self.inputDocument rangeOfComposedCharacterSequenceAtIndex:cursor - 1].location : 0;
        else if (right) cursor = !extend && self.documentSelection.length ? NSMaxRange(self.documentSelection) :
            cursor < self.inputDocument.length ? NSMaxRange([self.inputDocument rangeOfComposedCharacterSequenceAtIndex:cursor]) : cursor;
        else cursor = home ? 0 : self.inputDocument.length;
        if (!all && !extend) anchor = cursor;
        NSRange range = NSMakeRange(MIN(anchor, cursor), MAX(anchor, cursor) - MIN(anchor, cursor));
        if ([self emitDocumentEdit:range text:@"" selectionOnly:YES]) {
            int latest = (g_hjp.evHead + HJP_MAX_EVENTS - 1) % HJP_MAX_EVENTS;
            g_hjp.events[latest].edit.cursor = (int)[[self.inputDocument substringToIndex:cursor] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
            self.documentSelection = range;
            self.documentAnchor = anchor; self.documentCursor = cursor;
        }
        self.handledTextCommand = YES;
        return;
    }
    /* 折返し上下移動などGUI側で処理する操作の後は、次の描画まで範囲指定を使わない。 */
    self.documentID = 0;
    self.inputDocument = @"";
}
- (void)setMarkedText:(id)aString selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    NSRange range = self.documentID ? [self committedRange:replacementRange] : NSMakeRange(0, 0);
    if (range.location == NSNotFound) return;
    if (self.documentID && ![self emitDocumentEdit:range text:@"" selectionOnly:YES]) return;
    self.compositionBase = range;
    self.documentSelection = range;
    self.documentAnchor = range.location;
    self.documentCursor = NSMaxRange(range);
    self.composition = [aString isKindOfClass:[NSAttributedString class]] ? [aString string] : aString;
    NSUInteger length = self.composition.length;
    NSUInteger start = MIN(selectedRange.location, length);
    self.compositionSelection = NSMakeRange(start, MIN(selectedRange.length, length - start));
    HjpEvent e = {0};
    e.type = HJP_EVENT_TEXTEDITING;
    const char *utf8 = self.composition.UTF8String;
    size_t n = utf8 ? strlen(utf8) : 0;
    if (n >= sizeof(e.text.text)) {
        n = sizeof(e.text.text) - 1;
        while (n && ((unsigned char)utf8[n] & 0xc0) == 0x80) n--;
    }
    if (n) memcpy(e.text.text, utf8, n);
    hjp__pushEvent(&e);
}
- (void)clearComposition {
    BOOL hadComposition = self.hasMarkedText;
    self.composition = @"";
    self.compositionSelection = NSMakeRange(0, 0);
    if (hadComposition) {
        HjpEvent e = {0}; e.type = HJP_EVENT_TEXTEDITING; hjp__pushEvent(&e);
    }
}
- (void)unmarkText {
    if ([self hasMarkedText]) [self insertText:self.composition replacementRange:NSMakeRange(NSNotFound, 0)];
}
- (NSRange)selectedRange {
    if ([self hasMarkedText]) return NSMakeRange(self.compositionBase.location + self.compositionSelection.location, self.compositionSelection.length);
    return self.documentID ? self.documentSelection : NSMakeRange(NSNotFound, 0);
}
- (NSRange)markedRange { return [self hasMarkedText] ? NSMakeRange(self.compositionBase.location, self.composition.length) : NSMakeRange(NSNotFound, 0); }
- (BOOL)hasMarkedText { return self.composition.length > 0; }
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    NSString *document = [self documentWithComposition];
    if (range.location > document.length) return nil;
    NSRange valid = NSMakeRange(range.location, MIN(range.length, document.length - range.location));
    if (actualRange) *actualRange = valid;
    return [[NSAttributedString alloc] initWithString:[document substringWithRange:valid]];
}
- (NSArray *)validAttributesForMarkedText { return @[]; }
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    if (actualRange) *actualRange = range;
    NSRect rect = [self convertRect:self.inputRect toView:nil];
    return self.window ? [self.window convertRectToScreen:rect] : NSZeroRect;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point { (void)point; return NSNotFound; }

@end

/* =====================================================================
 * NSWindow サブクラス
 * ===================================================================*/
@interface HjpNSWindow : NSWindow
@end

@implementation HjpNSWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

/* =====================================================================
 * NSWindowDelegate
 * ===================================================================*/
@interface HjpWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation HjpWindowDelegate

- (BOOL)windowShouldClose:(id)sender {
    (void)sender;
    HjpEvent e = {0};
    e.type = HJP_EVENT_QUIT;
    hjp__pushEvent(&e);
    return NO;
}
- (void)windowDidResize:(NSNotification *)notification {
    (void)notification;
    HjpEvent e = {0};
    e.type = HJP_EVENT_WINDOWEVENT;
    e.window.event = HJP_WINDOWEVENT_SIZE_CHANGED;
    hjp__pushEvent(&e);
}
- (void)windowDidBecomeKey:(NSNotification *)notification {
    (void)notification;
    HjpEvent e = {0};
    e.type = HJP_EVENT_WINDOWEVENT;
    e.window.event = HJP_WINDOWEVENT_FOCUS_GAINED;
    hjp__pushEvent(&e);
}
- (void)windowDidResignKey:(NSNotification *)notification {
    (void)notification;
    HjpEvent e = {0};
    e.type = HJP_EVENT_WINDOWEVENT;
    e.window.event = HJP_WINDOWEVENT_FOCUS_LOST;
    hjp__pushEvent(&e);
}
@end

/* =====================================================================
 * NSApplicationDelegate
 * ===================================================================*/
@interface HjpAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation HjpAppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    [[NSRunningApplication currentApplication]
        activateWithOptions:NSApplicationActivateIgnoringOtherApps];
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender;
    HjpEvent e = {0};
    e.type = HJP_EVENT_QUIT;
    hjp__pushEvent(&e);
    return NSTerminateCancel;
}
@end

/* =====================================================================
 * 初期化/終了
 * ===================================================================*/
bool hjp_init(void) {
    if (g_hjp.initialized) return true;
    @autoreleasepool {
        [NSApplication sharedApplication];
        /* activationPolicy は finishLaunching より前に設定する必要がある (macOS 10.14+) */
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        g_hjp.appDelegate = [[HjpAppDelegate alloc] init];
        [NSApp setDelegate:g_hjp.appDelegate];
        /* メニューバー */
        NSMenu *menubar = [[NSMenu alloc] init];
        NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
        [menubar addItem:appMenuItem];
        [NSApp setMainMenu:menubar];
        NSMenu *appMenu = [[NSMenu alloc] init];
        [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
        [appMenuItem setSubmenu:appMenu];
        /* Finish launching */
        [NSApp finishLaunching];
        /* macOS 15: finishLaunching 後にrunloopを回して初期化を完了させる */
        {
            NSEvent *ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
            if (ev) [NSApp sendEvent:ev];
            [NSApp updateWindows];
        }
        mach_timebase_info(&g_hjp.timebase);
        g_hjp.startTime = mach_absolute_time();
        g_hjp.initialized = true;
    }
    return true;
}

void hjp_quit(void) {
    g_hjp.initialized = false;
}

/* =====================================================================
 * ウィンドウ
 * ===================================================================*/
HjpWindow *hjp_window_create(const char *title, int x, int y, int w, int h, uint32_t flags) {
    @autoreleasepool {
        HjpWindow *win = (HjpWindow*)calloc(1, sizeof(HjpWindow));
        if (!win) return NULL;
        win->windowId = ++g_hjp.nextWindowId;

        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
        if (flags & HJP_WINDOW_RESIZABLE) style |= NSWindowStyleMaskResizable;
        if (flags & HJP_WINDOW_BORDERLESS) style = NSWindowStyleMaskBorderless;

        NSRect frame;
        if (x == HJP_WINDOWPOS_CENTERED || y == HJP_WINDOWPOS_CENTERED) {
            NSScreen *screen = [NSScreen mainScreen];
            CGFloat sw = screen.frame.size.width, sh = screen.frame.size.height;
            frame = NSMakeRect((sw-w)/2, (sh-h)/2, w, h);
        } else {
            frame = NSMakeRect(x, y, w, h);
        }

        win->nswin = [[HjpNSWindow alloc]
            initWithContentRect:frame
            styleMask:style
            backing:NSBackingStoreBuffered
            defer:NO];
        if (!win->nswin) { free(win); return NULL; }

        win->delegate = [[HjpWindowDelegate alloc] init];
        [win->nswin setDelegate:win->delegate];
        [win->nswin setReleasedWhenClosed:NO];
        [win->nswin setTitle:[NSString stringWithUTF8String:title ? title : ""]];
        [win->nswin setAcceptsMouseMovedEvents:YES];

        /* OpenGL view */
        if (flags & HJP_WINDOW_OPENGL) {
            NSOpenGLPixelFormatAttribute attrs[] = {
                NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
                NSOpenGLPFAColorSize, 24,
                NSOpenGLPFAAlphaSize, 8,
                NSOpenGLPFADepthSize, 24,
                NSOpenGLPFAStencilSize, 8,
                NSOpenGLPFADoubleBuffer,
                NSOpenGLPFAAccelerated,
                0
            };
            if (flags & HJP_WINDOW_HIGHDPI) {
                /* wantsBestResolutionOpenGLSurface is set later */
            }
            NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
            win->glview = [[HjpOpenGLView alloc] initWithFrame:[[win->nswin contentView] bounds]
                                                   pixelFormat:pf];
            [win->glview setAutoresizingMask:NSViewWidthSizable|NSViewHeightSizable];
            if (flags & HJP_WINDOW_HIGHDPI)
                [win->glview setWantsBestResolutionOpenGLSurface:YES];
            [[win->nswin contentView] addSubview:win->glview];
            win->glctx = [win->glview openGLContext];
        }

        if (flags & HJP_WINDOW_SHOWN) {
            /* macOS 15 Sequoia: orderFrontRegardless でフォーカス制限を迂回 */
            [win->nswin setLevel:NSNormalWindowLevel];
            [win->nswin makeKeyAndOrderFront:nil];
            [win->nswin orderFrontRegardless];
            [win->nswin display];
            [[NSRunningApplication currentApplication]
                activateWithOptions:NSApplicationActivateIgnoringOtherApps];
            /* runloop を数回ポンプして Cocoa にウィンドウを描画させる */
            for (int _i = 0; _i < 5; _i++) {
                NSEvent *_ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                 untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                                                    inMode:NSDefaultRunLoopMode
                                                   dequeue:YES];
                if (_ev) [NSApp sendEvent:_ev];
                [NSApp updateWindows];
            }
        }

        g_hjp.mainWindow = win;
        win->glview.ownerWindow = win;
        return win;
    }
}

void hjp_window_destroy(HjpWindow *win) {
    if (!win) return;
    @autoreleasepool {
        for (HjpAccessibilityElement *element in win->glview.accessibleCache.allValues) element.live = NO;
        win->glview.ownerWindow = NULL;
        [win->nswin setDelegate:nil];
        if ([NSOpenGLContext currentContext] == win->glctx)
            [NSOpenGLContext clearCurrentContext];
        [win->glctx clearDrawable];
        [win->nswin close];
        // calloc/freeで管理する構造体のstrongフィールドは明示的に解放する。
        win->glctx = nil;
        win->glview = nil;
        win->nswin = nil;
        win->delegate = nil;
    }
    if (g_hjp.mainWindow == win) g_hjp.mainWindow = NULL;
    free(win);
}

void hjp_window_set_title(HjpWindow *win, const char *title) {
    @autoreleasepool { [win->nswin setTitle:[NSString stringWithUTF8String:title]]; }
}
void hjp_window_set_size(HjpWindow *win, int w, int h) {
    NSRect f = [win->nswin frame]; f.size = NSMakeSize(w, h);
    [win->nswin setFrame:f display:YES];
}
void hjp_window_get_size(HjpWindow *win, int *w, int *h) {
    NSRect f = [[win->nswin contentView] frame];
    if (w) *w = (int)f.size.width;
    if (h) *h = (int)f.size.height;
}
void hjp_window_set_position(HjpWindow *win, int x, int y) {
    [win->nswin setFrameOrigin:NSMakePoint(x, y)];
}
void hjp_window_maximize(HjpWindow *win) { [win->nswin zoom:nil]; }
void hjp_window_minimize(HjpWindow *win) { [win->nswin miniaturize:nil]; }
void hjp_window_set_fullscreen(HjpWindow *win, bool on) {
    BOOL isFS = ([win->nswin styleMask] & NSWindowStyleMaskFullScreen) != 0;
    if ((on && !isFS) || (!on && isFS)) [win->nswin toggleFullScreen:nil];
}
void hjp_window_set_opacity(HjpWindow *win, float op) { [win->nswin setAlphaValue:op]; }
void hjp_window_set_bordered(HjpWindow *win, bool on) {
    (void)win; (void)on;
    /* 動的にボーダー切り替えはmacOSでは制限あり */
}
void hjp_window_set_always_on_top(HjpWindow *win, bool on) {
    [win->nswin setLevel:on ? NSFloatingWindowLevel : NSNormalWindowLevel];
}
int hjp_window_get_id(HjpWindow *win) { return win->windowId; }

/* =====================================================================
 * OpenGL コンテキスト
 * ===================================================================*/
HjpGLContext hjp_gl_create_context(HjpWindow *win) {
    if (!win || !win->glctx) return NULL;
    [win->glctx makeCurrentContext];
    return (__bridge HjpGLContext)win->glctx;
}

void hjp_gl_delete_context(HjpGLContext ctx) {
    (void)ctx; /* NSOpenGLContext は ARC/autorelease で管理 */
}

void hjp_gl_make_current(HjpWindow *win, HjpGLContext ctx) {
    (void)ctx;
    if (win && win->glctx) [win->glctx makeCurrentContext];
}

void hjp_gl_set_swap_interval(int interval) {
    if (g_hjp.mainWindow && g_hjp.mainWindow->glctx) {
        GLint val = interval;
        [g_hjp.mainWindow->glctx setValues:&val forParameter:NSOpenGLContextParameterSwapInterval];
    }
}

void hjp_gl_swap_window(HjpWindow *win) {
    if (win && win->glctx) [win->glctx flushBuffer];
}

void hjp_gl_get_drawable_size(HjpWindow *win, int *w, int *h) {
    if (!win || !win->glview) {
        if (w) *w = 0; if (h) *h = 0; return;
    }
    NSRect bp = [win->glview convertRectToBacking:[win->glview bounds]];
    if (w) *w = (int)bp.size.width;
    if (h) *h = (int)bp.size.height;
}

/* =====================================================================
 * イベント
 * ===================================================================*/
bool hjp_poll_event(HjpEvent *event) {
    @autoreleasepool {
        /* まず Cocoa イベントをポンプ */
        NSEvent *nsev;
        while ((nsev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                          untilDate:nil
                                             inMode:NSDefaultRunLoopMode
                                            dequeue:YES])) {
            [NSApp sendEvent:nsev];
            [NSApp updateWindows];
        }
        /* キューからポップ */
        return hjp__popEvent(event);
    }
}

/* =====================================================================
 * タイマー
 * ===================================================================*/
HjpTicks hjp_get_ticks(void) {
    uint64_t elapsed = mach_absolute_time() - g_hjp.startTime;
    uint64_t ns = elapsed * g_hjp.timebase.numer / g_hjp.timebase.denom;
    return (HjpTicks)(ns / 1000000ULL);
}

void hjp_delay(uint32_t ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* =====================================================================
 * クリップボード
 * ===================================================================*/
char *hjp_get_clipboard_text(void) {
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSString *str = [pb stringForType:NSPasteboardTypeString];
        if (!str) return NULL;
        const char *utf8 = [str UTF8String];
        return utf8 ? strdup(utf8) : NULL;
    }
}

void hjp_set_clipboard_text(const char *text) {
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:[NSString stringWithUTF8String:text] forType:NSPasteboardTypeString];
    }
}

/* =====================================================================
 * テキスト入力
 * ===================================================================*/
void hjp_start_text_input(void) {
    /* macOS では常にテキスト入力可能、特別な処理なし */
}
void hjp_stop_text_input(void) {
    NSView *view = NSApp.keyWindow.contentView;
    if ([view isKindOfClass:[HjpOpenGLView class]]) {
        HjpOpenGLView *input = (HjpOpenGLView *)view;
        [input clearComposition];
        input.documentID = 0;
        input.inputDocument = @"";
    }
}

void hjp_set_text_input_context(HjpWindow *window, uint32_t id, const char *text,
                               int start, int end, int capacity) {
    HjpOpenGLView *view = window ? window->glview : nil;
    if (!view) return;
    if (!id) {
        if (view.hasMarkedText) [view clearComposition];
        view.documentID = 0;
        view.inputDocument = @"";
        return;
    }
    if (!text) return;
    size_t length = strlen(text);
    if (start < 0 || end < 0 || (size_t)start > length || (size_t)end > length || capacity <= 0 || length >= (size_t)capacity) return;
    NSString *document = [[NSString alloc] initWithUTF8String:text];
    NSString *prefix = [[NSString alloc] initWithBytes:text length:start encoding:NSUTF8StringEncoding];
    NSString *finish = [[NSString alloc] initWithBytes:text length:end encoding:NSUTF8StringEncoding];
    if (!document || !prefix || !finish) return;
    if (view.documentID != id || ![view.inputDocument isEqualToString:document]) {
        if (view.hasMarkedText) [view clearComposition];
        view.inputDocument = document;
    }
    view.documentID = id;
    view.documentCapacity = capacity;
    view.documentAnchor = prefix.length; view.documentCursor = finish.length;
    view.documentSelection = NSMakeRange(MIN(prefix.length, finish.length),
        MAX(prefix.length, finish.length) - MIN(prefix.length, finish.length));
}

void hjp_set_text_input_rect(HjpWindow *window, float x, float y, float w, float h) {
    if (!window || !window->glview) return;
    HjpOpenGLView *view = window->glview;
    float local_y = view.isFlipped ? y : view.bounds.size.height - y - h;
    view.inputRect = NSMakeRect(x, local_y, w, h);
    [view.inputContext invalidateCharacterCoordinates];
}

void hjp_accessibility_begin(HjpWindow *window) {
    HjpOpenGLView *view = window ? window->glview : nil;
    if (!view) return;
    if (!view.accessibleCache) view.accessibleCache = [NSMutableDictionary dictionary];
    if (!view.accessibleFrame) view.accessibleFrame = [NSMutableArray array];
    [view.accessibleFrame removeAllObjects];
    for (HjpAccessibilityElement *element in view.accessibleCache.allValues) {
        element.live = NO;
        element.accessibilityEnabled = NO;
    }
}
int hjp_accessibility_window_id(HjpWindow *window) {
    return window ? window->windowId : -1;
}
bool hjp_accessibility_announce(HjpWindow *window, const char *text) {
    if (!window || !window->glview || !text || !*text) return false;
    NSString *message = [NSString stringWithUTF8String:text];
    if (!message) return false;
    NSAccessibilityPostNotificationWithUserInfo(NSApp, NSAccessibilityAnnouncementRequestedNotification,
        @{NSAccessibilityAnnouncementKey: message, NSAccessibilityPriorityKey: @(NSAccessibilityPriorityMedium)});
    return true;
}
bool hjp_accessibility_add(HjpWindow *window, uint32_t id, const char *name, const char *help,
    int role, float x, float y, float w, float h, bool enabled, bool selected) {
    HjpOpenGLView *view = window ? window->glview : nil;
    if (!view || !view.accessibleCache || w <= 0 || h <= 0 || !isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h)) return false;
    NSNumber *key = @(id);
    HjpAccessibilityElement *element = view.accessibleCache[key];
    if (!element) {
        if (view.accessibleCache.count >= 512) {
            for (NSNumber *old in view.accessibleCache.allKeys) if (!view.accessibleCache[old].live) {
                [view.accessibleCache removeObjectForKey:old]; break;
            }
        }
        if (view.accessibleCache.count >= 512) return false;
        element = [[HjpAccessibilityElement alloc] init];
        element.widgetID = id;
        element.ownerView = view;
        view.accessibleCache[key] = element;
    }
    if (!element.live) [view.accessibleFrame addObject:element];
    element.live = YES;
    element.accessibilityElement = YES;
    element.accessibilityRole = role == 1 ? NSAccessibilityCheckBoxRole : role == 2 ? NSAccessibilityRadioButtonRole : NSAccessibilityButtonRole;
    element.accessibilityLabel = (name ? [NSString stringWithUTF8String:name] : nil) ?: @"";
    element.accessibilityHelp = (help ? [NSString stringWithUTF8String:help] : nil) ?: @"";
    element.accessibilityEnabled = enabled;
    element.accessibilityValue = role == 1 || role == 2 ? @(selected) : nil;
    element.accessibilityFrameInParentSpace = NSMakeRect(x, view.isFlipped ? y : view.bounds.size.height - y - h, w, h);
    return true;
}
void hjp_accessibility_end(HjpWindow *window) {
    HjpOpenGLView *view = window ? window->glview : nil;
    if (!view) return;
    for (NSNumber *key in [view.accessibleCache.allKeys copy])
        if (!view.accessibleCache[key].live) [view.accessibleCache removeObjectForKey:key];
    NSArray *next = [view.accessibleFrame copy];
    BOOL changed = ![view.accessiblePublished isEqualToArray:next];
    view.accessiblePublished = next;
    if (changed) NSAccessibilityPostNotification(view, NSAccessibilityLayoutChangedNotification);
}

/* =====================================================================
 * キーボード/マウス状態
 * ===================================================================*/
const uint8_t *hjp_get_keyboard_state(int *numkeys) {
    if (numkeys) *numkeys = HJP_MAX_KEYS;
    return g_hjp.keys;
}

uint32_t hjp_get_mouse_state(int *x, int *y) {
    if (x) *x = g_hjp.mouseX;
    if (y) *y = g_hjp.mouseY;
    return g_hjp.mouseButtons;
}

/* =====================================================================
 * カーソル
 * ===================================================================*/
HjpCursor hjp_create_system_cursor(int id) {
    @autoreleasepool {
        NSCursor *c = nil;
        switch (id) {
        case HJP_CURSOR_ARROW:     c = [NSCursor arrowCursor]; break;
        case HJP_CURSOR_HAND:      c = [NSCursor pointingHandCursor]; break;
        case HJP_CURSOR_IBEAM:     c = [NSCursor IBeamCursor]; break;
        case HJP_CURSOR_CROSSHAIR: c = [NSCursor crosshairCursor]; break;
        case HJP_CURSOR_SIZEALL:   c = [NSCursor openHandCursor]; break;
        case HJP_CURSOR_SIZEWE:    c = [NSCursor resizeLeftRightCursor]; break;
        case HJP_CURSOR_SIZENS:    c = [NSCursor resizeUpDownCursor]; break;
        default: c = [NSCursor arrowCursor]; break;
        }
#if __has_feature(objc_arc)
        return (__bridge_retained HjpCursor)c;
#else
        return (HjpCursor)[c retain];
#endif
    }
}

void hjp_set_cursor(HjpCursor cursor) {
    if (!cursor) return;
    NSCursor *c = (__bridge NSCursor *)cursor;
    [c set];
}

void hjp_free_cursor(HjpCursor cursor) {
    if (!cursor) return;
#if __has_feature(objc_arc)
    NSCursor *c = (__bridge_transfer NSCursor *)cursor;
    (void)c;
#else
    [(NSCursor *)cursor release];
#endif
}

/* =====================================================================
 * ディスプレイ
 * ===================================================================*/
int hjp_get_num_displays(void) {
    return (int)[[NSScreen screens] count];
}

void hjp_get_current_display_mode(int display_index, HjpDisplayMode *mode) {
    @autoreleasepool {
        NSArray *screens = [NSScreen screens];
        if (display_index < 0 || display_index >= (int)[screens count]) {
            if (mode) { mode->w = 0; mode->h = 0; mode->refresh_rate = 0; }
            return;
        }
        NSScreen *scr = screens[display_index];
        NSRect f = [scr frame];
        mode->w = (int)f.size.width;
        mode->h = (int)f.size.height;
        /* Refresh rate from CGDisplayMode */
        CGDirectDisplayID displayID = [[[scr deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
        CGDisplayModeRef dm = CGDisplayCopyDisplayMode(displayID);
        mode->refresh_rate = dm ? (int)CGDisplayModeGetRefreshRate(dm) : 60;
        if (dm) CGDisplayModeRelease(dm);
    }
}

void hjp_get_display_dpi(int display_index, float *ddpi, float *hdpi, float *vdpi) {
    @autoreleasepool {
        NSArray *screens = [NSScreen screens];
        if (display_index < 0 || display_index >= (int)[screens count]) {
            if (ddpi) *ddpi = 72.0f;
            if (hdpi) *hdpi = 72.0f;
            if (vdpi) *vdpi = 72.0f;
            return;
        }
        NSScreen *scr = screens[display_index];
        CGFloat bf = [scr backingScaleFactor];
        float dpi = 72.0f * (float)bf;
        if (ddpi) *ddpi = dpi;
        if (hdpi) *hdpi = dpi;
        if (vdpi) *vdpi = dpi;
    }
}

/* =====================================================================
 * メモリ
 * ===================================================================*/
void hjp_free(void *ptr) { free(ptr); }

/* =====================================================================
 * フォントレンダリング (CoreText)
 * ===================================================================*/

/* 内部フォントデータ */
typedef struct HjpFontData {
    CTFontDescriptorRef descriptor;
    CGDataProviderRef dataProvider;
    CGFontRef cgFont;
} HjpFontData;

HjpFont hjp_font_create_from_file(const char *path) {
    @autoreleasepool {
        NSString *nspath = [NSString stringWithUTF8String:path];
        NSURL *url = [NSURL fileURLWithPath:nspath];
        CGDataProviderRef dp = CGDataProviderCreateWithURL((__bridge CFURLRef)url);
        if (!dp) return NULL;
        CGFontRef cgFont = CGFontCreateWithDataProvider(dp);
        if (!cgFont) { CGDataProviderRelease(dp); return NULL; }
        CTFontDescriptorRef desc = CTFontManagerCreateFontDescriptorFromData(
            (__bridge CFDataRef)[NSData dataWithContentsOfURL:url]);
        HjpFontData *fd = (HjpFontData*)calloc(1, sizeof(HjpFontData));
        fd->dataProvider = dp;
        fd->cgFont = cgFont;
        fd->descriptor = desc;
        return (HjpFont)fd;
    }
}

HjpFont hjp_font_create_from_mem(const unsigned char *data, int ndata) {
    @autoreleasepool {
        CFDataRef cfdata = CFDataCreate(NULL, data, ndata);
        if (!cfdata) return NULL;
        CGDataProviderRef dp = CGDataProviderCreateWithCFData(cfdata);
        CGFontRef cgFont = CGFontCreateWithDataProvider(dp);
        CFRelease(cfdata);
        if (!cgFont) { CGDataProviderRelease(dp); return NULL; }
        CTFontDescriptorRef desc = CTFontManagerCreateFontDescriptorFromData(
            (__bridge CFDataRef)[NSData dataWithBytes:data length:ndata]);
        HjpFontData *fd = (HjpFontData*)calloc(1, sizeof(HjpFontData));
        fd->dataProvider = dp;
        fd->cgFont = cgFont;
        fd->descriptor = desc;
        return (HjpFont)fd;
    }
}

void hjp_font_destroy(HjpFont font) {
    if (!font) return;
    HjpFontData *fd = (HjpFontData*)font;
    if (fd->descriptor) CFRelease(fd->descriptor);
    if (fd->cgFont) CGFontRelease(fd->cgFont);
    if (fd->dataProvider) CGDataProviderRelease(fd->dataProvider);
    free(fd);
}

int hjp_font_get_glyph(HjpFont font, float size, uint32_t codepoint,
                        unsigned char **bitmap, int *w, int *h,
                        int *xoff, int *yoff, float *advance) {
    @autoreleasepool {
        if (!font) return 0;
        HjpFontData *fd = (HjpFontData*)font;
        CTFontRef ctFont = CTFontCreateWithGraphicsFont(fd->cgFont, size, NULL, fd->descriptor);
        if (!ctFont) return 0;

        /* コードポイント → グリフ */
        UniChar chars[2]; CGGlyph glyphs[2]; int charCount = 1;
        if (codepoint > 0xFFFF) {
            codepoint -= 0x10000;
            chars[0] = (UniChar)(0xD800 + (codepoint >> 10));
            chars[1] = (UniChar)(0xDC00 + (codepoint & 0x3FF));
            charCount = 2;
        } else {
            chars[0] = (UniChar)codepoint;
        }
        if (!CTFontGetGlyphsForCharacters(ctFont, chars, glyphs, charCount)) {
            CFRelease(ctFont);
            return 0;
        }

        CGGlyph glyph = glyphs[0];
        CGRect bbox = CTFontGetBoundingRectsForGlyphs(ctFont, kCTFontOrientationDefault, &glyph, NULL, 1);
        CGSize adv;
        CTFontGetAdvancesForGlyphs(ctFont, kCTFontOrientationDefault, &glyph, &adv, 1);

        int gw = (int)ceilf(bbox.size.width) + 2;
        int gh = (int)ceilf(bbox.size.height) + 2;
        if (gw < 1) gw = 1;
        if (gh < 1) gh = 1;

        /* ビットマップ描画 (グレースケール) */
        unsigned char *buf = (unsigned char*)calloc(1, gw * gh);
        CGColorSpaceRef grayCS = CGColorSpaceCreateDeviceGray();
        CGContextRef cgctx = CGBitmapContextCreate(buf, gw, gh, 8, gw, grayCS,
                                                    kCGImageAlphaNone);
        CGColorSpaceRelease(grayCS);
        if (!cgctx) { free(buf); CFRelease(ctFont); return 0; }

        CGContextSetGrayFillColor(cgctx, 1.0, 1.0);
        CGContextSetAllowsFontSmoothing(cgctx, false);
        CGContextSetShouldSmoothFonts(cgctx, false);
        CGContextSetAllowsAntialiasing(cgctx, true);
        CGContextSetShouldAntialias(cgctx, true);

        /* CTFontDrawGlyphs は macOS 14 で deprecated のため
         * CTFontCreatePathForGlyph でパスを取得してフィル (CG の Y-up 座標系) */
        CGPoint pos = CGPointMake(-bbox.origin.x + 1, -bbox.origin.y + 1);
        CGContextTranslateCTM(cgctx, pos.x, pos.y);
        CGPathRef glyphPath = CTFontCreatePathForGlyph(ctFont, glyph, NULL);
        if (glyphPath) {
            CGContextAddPath(cgctx, glyphPath);
            CGContextFillPath(cgctx);
            CGPathRelease(glyphPath);
        }

        /* CoreGraphics は Y-up → 上下反転して Y-down ビットマップに */
        unsigned char *flipped = (unsigned char*)malloc(gw * gh);
        for (int row = 0; row < gh; row++)
            memcpy(flipped + row * gw, buf + (gh - 1 - row) * gw, gw);

        CGContextRelease(cgctx);
        free(buf);
        CFRelease(ctFont);

        *bitmap = flipped;
        *w = gw; *h = gh;
        *xoff = (int)floorf(bbox.origin.x) - 1;
        *yoff = -(int)ceilf(bbox.origin.y + bbox.size.height) - 1;
        *advance = (float)adv.width;
        return 1;
    }
}

void hjp_font_metrics(HjpFont font, float size,
                      float *ascent, float *descent, float *line_gap) {
    @autoreleasepool {
        if (!font) {
            if (ascent) *ascent = size * 0.8f;
            if (descent) *descent = -size * 0.2f;
            if (line_gap) *line_gap = size * 0.2f;
            return;
        }
        HjpFontData *fd = (HjpFontData*)font;
        CTFontRef ctFont = CTFontCreateWithGraphicsFont(fd->cgFont, size, NULL, fd->descriptor);
        if (!ctFont) {
            if (ascent) *ascent = size * 0.8f;
            if (descent) *descent = -size * 0.2f;
            if (line_gap) *line_gap = size * 0.2f;
            return;
        }
        if (ascent) *ascent = (float)CTFontGetAscent(ctFont);
        if (descent) *descent = -(float)CTFontGetDescent(ctFont);
        if (line_gap) *line_gap = (float)CTFontGetLeading(ctFont);
        CFRelease(ctFont);
    }
}

float hjp_font_text_width(HjpFont font, float size, const char *str, const char *end) {
    @autoreleasepool {
        if (!font || !str) return 0;
        HjpFontData *fd = (HjpFontData*)font;
        CTFontRef ctFont = CTFontCreateWithGraphicsFont(fd->cgFont, size, NULL, fd->descriptor);
        if (!ctFont) return 0;

        size_t len = end ? (size_t)(end - str) : strlen(str);
        NSString *nsstr = [[NSString alloc] initWithBytes:str length:len encoding:NSUTF8StringEncoding];
        if (!nsstr) { CFRelease(ctFont); return 0; }

        NSDictionary *attrs = @{(__bridge id)kCTFontAttributeName: (__bridge id)ctFont};
        NSAttributedString *attrStr = [[NSAttributedString alloc] initWithString:nsstr attributes:attrs];
        CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attrStr);
        double width = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
        CFRelease(line);
        CFRelease(ctFont);
        return (float)width;
    }
}

/* =====================================================================
 * 画像読み込み (ImageIO)
 * ===================================================================*/
unsigned char *hjp_image_load_mem(const unsigned char *data, int ndata, int *w, int *h) {
    @autoreleasepool {
        CFDataRef cfdata = CFDataCreate(NULL, data, ndata);
        if (!cfdata) return NULL;
        CGImageSourceRef src = CGImageSourceCreateWithData(cfdata, NULL);
        CFRelease(cfdata);
        if (!src) return NULL;

        CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
        CFRelease(src);
        if (!img) return NULL;

        size_t iw = CGImageGetWidth(img);
        size_t ih = CGImageGetHeight(img);
        *w = (int)iw; *h = (int)ih;

        unsigned char *pixels = (unsigned char*)malloc(iw * ih * 4);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef cgctx = CGBitmapContextCreate(pixels, iw, ih, 8, iw*4, cs,
            kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(cs);
        if (!cgctx) { free(pixels); CGImageRelease(img); return NULL; }

        CGContextDrawImage(cgctx, CGRectMake(0, 0, iw, ih), img);
        CGContextRelease(cgctx);
        CGImageRelease(img);
        return pixels;
    }
}

void hjp_image_free(unsigned char *pixels) {
    free(pixels);
}
