"""Cross-platform monitor bounds, with an SDL fallback when enumeration fails."""
from dataclasses import dataclass


@dataclass(frozen=True)
class Display:
    key: str
    label: str
    x: int
    y: int
    width: int
    height: int


def list_displays():
    try:
        from screeninfo import get_monitors
        monitors = get_monitors()
        result = []
        for i, m in enumerate(monitors):
            name = m.name or f"Display {i + 1}"
            # Position is intentionally excluded: moving a display preserves binding.
            key = f"{name}:{m.width_mm}x{m.height_mm}"
            if any(d.key == key for d in result):
                key += f":{i}"
            result.append(Display(key, f"{i + 1}: {name} · {m.width} × {m.height}",
                                  m.x, m.y, m.width, m.height))
        if result:
            return result
    except Exception as exc:
        print(f"[Displays] Monitor enumeration unavailable: {exc}")
    import pygame
    w, h = pygame.display.get_desktop_sizes()[0]
    return [Display("primary", f"Primary · {w} × {h}", 0, 0, w, h)]


def deck_order(raw, count):
    """Keep the physical deck numbers; two-deck mode uses decks 1 and 2."""
    try:
        values = tuple(int(x.strip()) - 1 for x in raw.split(","))
    except (ValueError, AttributeError):
        raise ValueError("Deck order must contain 1, 2, 3, 4 exactly once")
    if sorted(values) != [0, 1, 2, 3]:
        raise ValueError("Deck order must contain 1, 2, 3, 4 exactly once")
    return tuple(i for i in values if i < count)
