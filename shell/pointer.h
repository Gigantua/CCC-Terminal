#pragma once
#include <algorithm>
#include <iterator>
#include <string>

enum class Pointer { Text, Arrow, Hand, Cross, Wait, Progress, Help, Forbidden, Move, ResizeWE, ResizeNS, ResizeNESW, ResizeNWSE };

struct PointerName { const char* name; Pointer shape; };

inline const PointerName* pointerNames(size_t& count) {
    static const PointerName names[] = {
        { "text", Pointer::Text }, { "xterm", Pointer::Text }, { "ibeam", Pointer::Text }, { "vertical-text", Pointer::Text },
        { "default", Pointer::Arrow }, { "left_ptr", Pointer::Arrow }, { "context-menu", Pointer::Arrow }, { "alias", Pointer::Arrow }, { "copy", Pointer::Arrow },
        { "pointer", Pointer::Hand }, { "hand", Pointer::Hand }, { "hand2", Pointer::Hand }, { "grab", Pointer::Hand }, { "grabbing", Pointer::Move },
        { "crosshair", Pointer::Cross }, { "cell", Pointer::Cross }, { "wait", Pointer::Wait }, { "watch", Pointer::Wait }, { "progress", Pointer::Progress },
        { "help", Pointer::Help }, { "question_arrow", Pointer::Help }, { "not-allowed", Pointer::Forbidden }, { "no-drop", Pointer::Forbidden },
        { "move", Pointer::Move }, { "all-scroll", Pointer::Move }, { "fleur", Pointer::Move },
        { "ew-resize", Pointer::ResizeWE }, { "col-resize", Pointer::ResizeWE }, { "e-resize", Pointer::ResizeWE }, { "w-resize", Pointer::ResizeWE },
        { "ns-resize", Pointer::ResizeNS }, { "row-resize", Pointer::ResizeNS }, { "n-resize", Pointer::ResizeNS }, { "s-resize", Pointer::ResizeNS },
        { "nesw-resize", Pointer::ResizeNESW }, { "ne-resize", Pointer::ResizeNESW }, { "sw-resize", Pointer::ResizeNESW },
        { "nwse-resize", Pointer::ResizeNWSE }, { "nw-resize", Pointer::ResizeNWSE }, { "se-resize", Pointer::ResizeNWSE },
    };
    count = std::size(names);
    return names;
}

inline const PointerName* findPointer(const std::string& name) {
    size_t n;
    const PointerName* names = pointerNames(n);
    const PointerName* at = std::find_if(names, names + n, [&](const PointerName& p) { return name == p.name; });
    return at == names + n ? nullptr : at;
}

inline Pointer pointerNamed(const std::string& spec) {
    std::string name = spec.substr(0, spec.find(','));
    const PointerName* p = findPointer(name);
    return p ? p->shape : Pointer::Text;
}
