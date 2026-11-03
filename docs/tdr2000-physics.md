# Carmageddon TDR 2000: what the executable says about the car physics

Analysis of the user's own `TDR2000.exe` (32-bit x86, 2.7 MB, no symbols, image base 0x400000), done locally with the LLVM
tools of the emsdk (`llvm-objdump -d --x86-asm-syntax=intel`) plus small Node scripts. Nothing from the game is copied into the
project. Addresses are virtual addresses. "Confirmed" = read directly from the code; "inferred" = my reading of it.

## Entry points that were useful
- Strings with source paths: `D:\work\library\collision.cpp`, `hierarchy.cpp`, `Lex.cpp`. Error strings such as
  "Wheel %d Offset instance node is invalid" (code at 0x53CEE9 / 0x53D560 / 0x53D635) lead to the car setup.
- Debug console command "carSuspension damp = %f spring = %f" (code at 0x4AA3A0): scales the car's suspension through two
  floats of the car object (+0x5C0, +0x5C4).
- Car class (`this` = ebx in the setup code): wheel array at +0x3F0 (descriptor side, 0x3A8 bytes per entry) and +0x3E0 (physics
  side, 0x18C bytes per entry), wheel count +0x3EC / +0x3D8, body (dynamic object) at +0x94.

## The car descriptor is read by position (setup code 0x53C800 - 0x53DB00)
Confirmed order, matching `Eagle4descriptor.txt`:
1. Gearbox: number of gears N, then N + 1 floats are stored at +0x43C: index 0 = reverse, index 1 = neutral (0, not read),
   index 2.. = forward ratios. Then: differential ratio (+0x440), clutch torque (+0x3B8), engine inertia (+0x3AC), gearbox
   inertia (+0x3B4), engine idle revs times a constant (+0x3A8).
2. Function files by name: torque curve (object at +0x444), then the tyre slip, tyre load, camber thrust, camber grip, front and
   rear down force and roll resistance curves (objects at +0x460, +0x47C, ...).
3. Per wheel, after its two node names: mass (+0xE0), x-axis inertia (+0xDC), suspension direction (+0x98..0xA0, normalised),
   **maximum suspension offset (+0x88), minimum suspension offset (+0x8C)**, camber in degrees (+0xD4, turned into a rotation),
   radius (+0xD8), **spring stiffness (+0x90), damper (+0x94)**. The wheel position is made relative to the body's +0xF4 vector.

## Suspension force (code at 0x543415, per wheel)
Confirmed: `force = (offset - minOffset) * spring * [car+0x5B8]` where offset is the wheel record's +0x84; the damper term is
`-(velocity of the mount along the suspension direction) * damper * [car+0x5B4]`; the sum is stored at +0x128 and, unless a flag
(+0x5C5) is set, clamped to +-(body field +0x100 * 9.81). Inferred: +0x100 of the body is the mass, so one suspension force
cannot exceed the car's weight, and the spring is free at the minimum offset (0.1 m for the Eagle) with up to 0.4 m of travel.

## Drive train (code at 0x543040)
Confirmed: engine speed comes from the gear ratio, the differential ratio and the driven wheels' speed; the torque curve object
is evaluated at that speed; the result is multiplied by the throttle (+0x80), a scale (+0x3BC), the differential and the gear ratio,
and shared between the driven wheels (division by their count). Brake torque per wheel is `brake input (+0x84) * 2000`.
Constants: 9.81 gravity, 0.0011111 (1/900), 900, 100000, 2000.

## .func files (code 0x59A1C0 load, 0x59A570 evaluate)
Piecewise functions: the first byte 1 = cubic (4 coefficients per point), else linear (value and slope per point); two
length-prefixed axis labels follow, then a header and 32-byte point records of doubles. Layout of the header not decoded yet.

## .dcol (chassis) header and the centre of mass (body constructor 0x51E180, confirmed from the code)
`this` = the body (+0x100 mass, +0xF4..0xFC centre vector), edi = the file's bytes. Mass = float 0 (1550 kg, also stored as 1/mass
at +0x104). Floats 1-3 (bytes 4, 8, 12: 1.8, 1.25, 3.96) go with the mass to 0x582770, which builds the inertia tensor of a box
(`m / 12 * (a^2 + b^2)`, inverted for +0x108...). Bytes 20, 24, 28 (x, y, z = 0, -0.2, 0.1 for the Eagle) are copied to
body +0xF4/+0xF8/+0xFC. Bytes 32.. are the hull (a count at byte 60, then points) handed to 0x57C3D0.
There is no computation: the centre of mass is data. The vector is added to every part position (wheel hub +0xF4 at 0x53D9C7,
the body position at 0x51ED9E), so it is the car origin seen from the centre of mass: **centre of mass = origin - vector**, i.e.
(0, +0.2, -0.1) in model space for the Eagle (x mirrored in our scene; zero here). The model origin is at axle height (the hull
spans y = -0.2 .. 1.015 and the road is about 0.45 below the origin), so the centre of mass is about 0.65 m above the road and
0.1 m behind the origin. The earlier reading (a permutation of floats 6-8 as fractions of the hull box) was wrong.

## Surface friction (scol materials)
The `.h` next to a `.scol` lists `#define NAME index` (index = polygon flags & 0xFFFF). NAME minus its trailing digits matches
`Attribute_<NAME>.txt` in ATTRIBUTES.dir/.pak (e.g. ROAD2 -> Attribute_Road.txt); names with no file (MAT126, DEFAULT23) get 1.
WHEEL_FRICTION values: road/concrete/metal/freeway/rock/wood 1.0, gravel 0.9, PoliceGrass 0.9, militarydirtroad 0.95, grass/dirt
0.8, sand 0.8, YellowSand 0.5, SlipperyRock 0.0. The minigame multiplies the grip by it (`surfaceFrictionEffect`).

## Tyre model (code 0x5433F3 - 0x544FA0, per wheel)
Confirmed: the contact point is the hub minus the radius (road level); the force is applied there. Slip vector = contact point
velocity relative to the surface, plus the wheel's peripheral speed along its heading. slip = |slip vector| / max(|contact speed|, 3.5).
mu = slipCurve(slip) * surface WHEEL_FRICTION (record +0x24) * tyre grade factor * camberGrip(angle). Friction force =
mu * body mass * 2.45 (about g / 4): the static quarter weight, NOT the wheel's dynamic load. A viscous term of
100 * DAMPENING (record +0x20) * tangential speed is added. Attribute keyword table (0x603370): WHEEL_FRICTION, DAMPENING,
BUMPYNESS, FFB_BUMPYNESS, HARDNESS, COLLISION_FRICTION, SPARKYNESS.
Inferred: because the friction does not follow the load, load transfer cannot feed back into grip, so there is no wheelie from
traction and the sideways limit is mu * g, which keeps the car from rolling over.
Not resolved: the Eagle slip curve reaches about 10, so some divisor/normalisation is missing; the minigame uses the curve shape
normalised to a peak of 1 times `grip`. Surface WHEEL_FRICTION per scol material is not wired in yet.

## Open
- Decode the .func header to read the torque curve (the force scale of the engine) and the tyre slip curve.
- Find the .dcol reader and the tyre force code (tyre slip / load / camber curves are used there).
- Settle what "offset" is geometrically (compression from the free position versus distance from the mount).
