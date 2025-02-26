// Aseprite
// Copyright (C) 2021-2022  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/app.h"
#include "app/context.h"
#include "app/context_observer.h"
#include "app/doc.h"
#include "app/doc_undo.h"
#include "app/doc_undo_observer.h"
#include "app/pref/preferences.h"
#include "app/script/docobj.h"
#include "app/script/engine.h"
#include "app/script/luacpp.h"
#include "doc/document.h"
#include "doc/sprite.h"
#include "ui/app_state.h"

#include <cstring>
#include <map>
#include <memory>

namespace app {
namespace script {

using namespace doc;

namespace {

using EventListener = int;

class AppEvents;
class SpriteEvents;
static std::unique_ptr<AppEvents> g_appEvents;
static std::map<doc::ObjectId, std::unique_ptr<SpriteEvents>> g_spriteEvents;

class Events {
public:
  using EventType = int;

  Events() { }
  virtual ~Events() { }
  Events(const Events&) = delete;
  Events& operator=(const Events&) = delete;

  virtual EventType eventType(const char* eventName) const = 0;

  bool hasListener(EventListener callbackRef) const {
    for (auto& listeners : m_listeners) {
      for (EventListener listener : listeners) {
        if (listener == callbackRef)
          return true;
      }
    }
    return false;
  }

  void add(EventType eventType, EventListener callbackRef) {
    if (eventType >= m_listeners.size())
      m_listeners.resize(eventType+1);

    auto& listeners = m_listeners[eventType];
    listeners.push_back(callbackRef);
    if (listeners.size() == 1)
      onAddFirstListener(eventType);
  }

  void remove(EventListener callbackRef) {
    for (int i=0; i<int(m_listeners.size()); ++i) {
      EventListeners& listeners = m_listeners[i];
      auto it = listeners.begin();
      auto end = listeners.end();
      bool removed = false;
      for (; it != end; ) {
        if (*it == callbackRef) {
          removed = true;
          it = listeners.erase(it);
          end = listeners.end();
        }
        else
          ++it;
      }
      if (removed && listeners.empty())
        onRemoveLastListener(i);
    }
  }

protected:
  void call(EventType eventType) {
    if (eventType >= m_listeners.size())
      return;

    script::Engine* engine = App::instance()->scriptEngine();
    lua_State* L = engine->luaState();

    try {
      for (EventListener callbackRef : m_listeners[eventType]) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, callbackRef);
        if (lua_pcall(L, 0, 0, 0)) {
          if (const char* s = lua_tostring(L, -1))
            engine->consolePrint(s);
        }
      }
    }
    catch (const std::exception& ex) {
      engine->consolePrint(ex.what());
    }
  }

private:
  virtual void onAddFirstListener(EventType eventType) = 0;
  virtual void onRemoveLastListener(EventType eventType) = 0;

  using EventListeners = std::vector<EventListener>;
  std::vector<EventListeners> m_listeners;
};

class AppEvents : public Events
                , private ContextObserver {
public:
  enum : EventType { Unknown = -1, SiteChange, FgColorChange, BgColorChange };

  AppEvents() {
  }

  EventType eventType(const char* eventName) const override {
    if (std::strcmp(eventName, "sitechange") == 0)
      return SiteChange;
    else if (std::strcmp(eventName, "fgcolorchange") == 0)
      return FgColorChange;
    else if (std::strcmp(eventName, "bgcolorchange") == 0)
      return BgColorChange;
    else
      return Unknown;
  }

private:

  void onAddFirstListener(EventType eventType) override {
    switch (eventType) {
      case SiteChange:
        App::instance()->context()->add_observer(this);
        break;
      case FgColorChange:
        m_fgConn = Preferences::instance().colorBar.fgColor
          .AfterChange.connect([this]{ onFgColorChange(); });
        break;
      case BgColorChange:
        m_bgConn = Preferences::instance().colorBar.bgColor
          .AfterChange.connect([this]{ onBgColorChange(); });
        break;
    }
  }

  void onRemoveLastListener(EventType eventType) override {
    switch (eventType) {
      case SiteChange:
        App::instance()->context()->remove_observer(this);
        break;
      case FgColorChange:
        m_fgConn.disconnect();
        break;
      case BgColorChange:
        m_bgConn.disconnect();
        break;
    }
  }

  void onFgColorChange() {
    call(FgColorChange);
  }

  void onBgColorChange() {
    call(BgColorChange);
  }

  // ContextObserver impl
  void onActiveSiteChange(const Site& site) override {
    call(SiteChange);
  }

  obs::scoped_connection m_fgConn;
  obs::scoped_connection m_bgConn;
};

class SpriteEvents : public Events
                   , public DocUndoObserver
                   , public DocObserver {
public:
  enum : EventType { Unknown = -1, Change, FilenameChange };

  SpriteEvents(const Sprite* sprite)
    : m_spriteId(sprite->id()) {
    doc()->add_observer(this);
  }

  ~SpriteEvents() {
    auto doc = this->doc();
    ASSERT(doc || ui::get_app_state() == ui::AppState::kClosingWithException);
    if (doc) {
      disconnectFromUndoHistory(doc);
      doc->remove_observer(this);
    }
  }

  EventType eventType(const char* eventName) const override {
    if (std::strcmp(eventName, "change") == 0)
      return Change;
    else if (std::strcmp(eventName, "filenamechange") == 0)
      return FilenameChange;
    else
      return Unknown;
  }

  // DocObserver impl
  void onCloseDocument(Doc* doc) override {
    auto it = g_spriteEvents.find(m_spriteId);
    ASSERT(it != g_spriteEvents.end());
    if (it != g_spriteEvents.end()) {
      // As this is an unique_ptr, here we are calling ~SpriteEvents()
      g_spriteEvents.erase(it);
    }
  }

  void onFileNameChanged(Doc* doc) override { call(FilenameChange); }

  // DocUndoObserver impl
  void onAddUndoState(DocUndo* history) override { call(Change);  }
  void onCurrentUndoStateChange(DocUndo* history) override { call(Change); }

private:

  void onAddFirstListener(EventType eventType) override {
    switch (eventType) {
      case Change:
        ASSERT(!m_observingUndo);
        doc()->undoHistory()->add_observer(this);
        m_observingUndo = true;
        break;
    }
  }

  void onRemoveLastListener(EventType eventType) override {
    switch (eventType) {
      case Change: {
        disconnectFromUndoHistory(doc());
        break;
      }
    }
  }

  Doc* doc() {
    Sprite* sprite = doc::get<Sprite>(m_spriteId);
    if (sprite)
      return static_cast<Doc*>(sprite->document());
    else
      return nullptr;
  }

  void disconnectFromUndoHistory(Doc* doc) {
    if (m_observingUndo) {
      doc->undoHistory()->remove_observer(this);
      m_observingUndo = false;
    }
  }

  ObjectId m_spriteId;
  bool m_observingUndo = false;
};

int Events_on(lua_State* L)
{
  auto evs = get_ptr<Events>(L, 1);
  const char* eventName = lua_tostring(L, 2);
  if (!eventName)
    return 0;

  const int type = evs->eventType(eventName);
  if (type < 0)
    return luaL_error(L, "invalid event name to listen");

  if (!lua_isfunction(L, 3))
    return luaL_error(L, "second argument must be a function");

  // Copy the callback function to add it to the global registry
  lua_pushvalue(L, 3);
  int callbackRef = luaL_ref(L, LUA_REGISTRYINDEX);
  evs->add(type, callbackRef);

  // Return the callback ref (this is an EventListener easier to use
  // in Events_off())
  lua_pushinteger(L, callbackRef);
  return 1;
}

int Events_off(lua_State* L)
{
  auto evs = get_ptr<Events>(L, 1);
  int callbackRef = LUA_REFNIL;

  // Remove by listener value
  if (lua_isinteger(L, 2)) {
    callbackRef = lua_tointeger(L, 2);
  }
  // Remove by function reference
  else if (lua_isfunction(L, 2)) {
    lua_pushnil(L);
    while (lua_next(L, LUA_REGISTRYINDEX) != 0) {
      if (lua_isnumber(L, -2) &&
          lua_isfunction(L, -1)) {
        int i = lua_tointeger(L, -2);
        if (// Compare value=function in 2nd argument
            lua_compare(L, -1, 2, LUA_OPEQ) &&
            // Check that this Events contain this reference
            evs->hasListener(i)) {
          callbackRef = i;
          lua_pop(L, 2);  // Pop value and key
          break;
        }
      }
      lua_pop(L, 1); // Pop the value, leave the key for next lua_next()
    }
  }
  else {
    return luaL_error(L, "first argument must be a function or a EventListener");
  }

  if (callbackRef != LUA_REFNIL &&
      // Check that we are removing a listener from this Events and no
      // other random value from the Lua registry
      evs->hasListener(callbackRef)) {
    evs->remove(callbackRef);
    luaL_unref(L, LUA_REGISTRYINDEX, callbackRef);
  }
  return 0;
}

// We don't need a __gc (to call ~Events()), because Events instances
// will be deleted when the Sprite is deleted or on App Exit
const luaL_Reg Events_methods[] = {
  { "on", Events_on },
  { "off", Events_off },
  { nullptr, nullptr }
};

} // anonymous namespace

DEF_MTNAME(Events);

void register_events_class(lua_State* L)
{
  REG_CLASS(L, Events);
}

void push_app_events(lua_State* L)
{
  if (!g_appEvents) {
    App::instance()->Exit.connect([]{ g_appEvents.reset(); });
    g_appEvents.reset(new AppEvents);
  }
  push_ptr<Events>(L, g_appEvents.get());
}

void push_sprite_events(lua_State* L, Sprite* sprite)
{
  // Clear the g_spriteEvents map on Exit() signal because if the dtor
  // is called in the normal C++ order destruction sequence by
  // compilation units, it could crash because each ~SpriteEvents()
  // needs the doc::get() function, which uses the "objects"
  // collection from "src/doc/objects.cpp" (so we cannot garantize
  // that that "objects" collection will be destroyed after
  // "g_spriteEvents")
  static bool atExit = false;
  if (!atExit) {
    atExit = true;
    App::instance()->Exit.connect([]{ g_spriteEvents.clear(); });
  }

  ASSERT(sprite);

  SpriteEvents* spriteEvents;

  auto it = g_spriteEvents.find(sprite->id());
  if (it != g_spriteEvents.end())
    spriteEvents = it->second.get();
  else {
    spriteEvents = new SpriteEvents(sprite);
    g_spriteEvents[sprite->id()].reset(spriteEvents);
  }

  push_ptr<Events>(L, spriteEvents);
}

} // namespace script
} // namespace app
