"""Validated physical deck ordering shared by the native display and profiles."""

def deck_order(raw, count):
    """Keep the physical deck numbers; two-deck mode uses decks 1 and 2."""
    try:
        values = tuple(int(x.strip()) - 1 for x in raw.split(","))
    except (ValueError, AttributeError):
        raise ValueError("Deck order must contain 1, 2, 3, 4 exactly once")
    if sorted(values) != [0, 1, 2, 3]:
        raise ValueError("Deck order must contain 1, 2, 3, 4 exactly once")
    return tuple(i for i in values if i < count)
