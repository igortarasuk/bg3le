# Looking for `ls::GlobalSwitches`

Written 2026-09-24. The object is findable; reading it through bg3se's
declared layout is not safe, so `Ext.Utils.GetGlobalSwitches` still refuses —
but it refuses on evidence now, and `src/vendor/global_switches.cpp` is the
search that produced it.

It matters twice over: it is one refusing `Ext.*`, and it is also the second
thing blocking bg3se's ImGui overlay, whose `InitializeUI` reads
`GetGlobalSwitches()->Language`.

## The anchor works

`Language` is one of Larian's sixteen-byte strings holding a short value, so
an English install has exactly this somewhere in it:

    45 6e 67 6c 69 73 68 00 00 00 00 00 00 00 00 07
    E  n  g  l  i  s  h                          len

Fifteen characters inline and the length in the last byte — a sixteen-byte
needle with no wildcards, which is far better than scanning for the text. A
scan finds 130–190 of them across the process, so a hit is a candidate: the
object would start `offsetof(Language)` before it.

## Boolean density is not a test

`GlobalSwitches` declares 91 boolean members, and a bool is 0 or 1. That
looked like a strong check — 91 independent one-bit tests — and it is not,
because a settings object is not the only thing made of small bytes. Accepting
nine in ten gave a base with 82 of 91 agreeing whose scalars were plainly
wrong:

    UIScaling          1060
    MouseSensitivity   -1158458304        <- a float's bits read as an int
    MaxNrOfAutoSaves   0
    CanAutoSave        false

Requiring all 91 rejected everything. Between runs the best score moved
between 66, 82 and 87 at different addresses, which is what a test with no
discriminating power looks like.

## Floats are a test

A float is thirty-two bits and almost all of them are meaningless. A setting
is a small finite number; random bytes are overwhelmingly NaN, infinite,
denormal or astronomical. `GlobalSwitches` declares twenty-odd floats —
`FadeSpeed`, `GameCameraRotation`, the camera speeds and the controller
thresholds — and requiring every one to be finite with a magnitude between
1e-6 and 1e6 is a real filter.

With both tests applied, no candidate in the process passes. The best after
the float filter scores 66 of 91 booleans.

## What that means

bg3se's `GlobalSwitches` is a Windows reverse-engineering: two thirds of its
members are named `field_NN`, and it contains `TranslatedString`, `HashSet`
and `STDString` members whose sizes differ on this build. So the declared
offsets are not this struct's offsets, and an address reported against them
would read the wrong fields — the "plausible wrong answer" this project
refuses to give.

One detail is worth keeping for whoever picks this up: the disagreeing
booleans are consistent across runs and start at the same place.

    +208   ShowLocalizationMarkers
    +212   EnablePortmapping
    +228   CrossplayEnabled
    +229   CrossplayInUse

The same four, run after run, at the same offsets, on the candidate whose
language reads "English". That consistency says something structural sits just
before +208 with a different size here, rather than that the search is finding
noise. Establishing what, the way `STDString` and `Module` were established,
is the way in — and it would want a live dump of the bytes around a confirmed
base, which needs a confirmed base first.

## What the code does now

`bg3le_global_switches()` runs the search once, requires every float to be
plausible and every boolean to be 0 or 1, and returns null otherwise. On
failure it logs the best candidate, its language, its score and the
disagreeing members by name and offset, so the next attempt starts from a
measurement rather than from scratch.
