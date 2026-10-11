#!/usr/bin/env python3
"""Generate Single Editor's versioned standard Ambisonics matrix artifacts."""

from __future__ import annotations

import argparse
import hashlib
import math
import struct
from dataclasses import dataclass
from pathlib import Path


GENERATOR_BACKEND = "singleeditor-sampled-maxre"
GENERATOR_VERSION = "1.0.0"
WEIGHT_PROFILE = "max-re-137.9deg-v1"
STATIC_NORMALIZATION = "fixed-reference-v1"
MAX_OUTPUTS = 12
MAX_INPUTS = 16
ROLE_VALUES = {
    "M": 2,
    "L": 3,
    "R": 4,
    "C": 5,
    "LFE": 6,
    "Ls": 7,
    "Rs": 8,
    "Lss": 9,
    "Rss": 10,
    "Lsr": 11,
    "Rsr": 12,
    "Ltf": 19,
    "Rtf": 20,
    "Ltr": 21,
    "Rtr": 22,
}


@dataclass(frozen=True)
class Speaker:
    stable_id: str
    label: str
    role: str
    azimuth: float
    elevation: float
    distance: float = 1.0
    weight: float = 1.0


@dataclass(frozen=True)
class Profile:
    profile_id: str
    display_name: str
    dimensionality: str
    speakers: tuple[Speaker, ...]


def speaker(role: str, azimuth: float, elevation: float = 0.0) -> Speaker:
    return Speaker(role.lower(), role, role, azimuth, elevation)


PROFILES = (
    Profile("standard-mono-v1", "Standard Mono", "ring2d", (speaker("M", 0.0),)),
    Profile("standard-stereo-v1", "Standard Stereo", "ring2d",
            (speaker("L", 30.0), speaker("R", -30.0))),
    Profile("standard-quad-v1", "Standard Quad", "ring2d",
            (speaker("L", 30.0), speaker("R", -30.0),
             speaker("Ls", 100.0), speaker("Rs", -100.0))),
    Profile("standard-5.1-v1", "Standard 5.1", "ring2d",
            (speaker("L", 30.0), speaker("R", -30.0), speaker("C", 0.0),
             speaker("LFE", 0.0), speaker("Ls", 100.0), speaker("Rs", -100.0))),
    Profile("standard-7.1-v1", "Standard 7.1", "ring2d",
            (speaker("L", 30.0), speaker("R", -30.0), speaker("C", 0.0),
             speaker("LFE", 0.0), speaker("Lss", 100.0), speaker("Rss", -100.0),
             speaker("Lsr", 145.0), speaker("Rsr", -145.0))),
    Profile("standard-7.1.4-v1", "Standard 7.1.4", "sphere3d",
            (speaker("L", 30.0), speaker("R", -30.0), speaker("C", 0.0),
             speaker("LFE", 0.0), speaker("Lss", 100.0), speaker("Rss", -100.0),
             speaker("Lsr", 145.0), speaker("Rsr", -145.0),
             speaker("Ltf", 30.0, 45.0), speaker("Rtf", -30.0, 45.0),
             speaker("Ltr", 145.0, 45.0), speaker("Rtr", -145.0, 45.0))),
)


def f32(value: float) -> float:
    rounded = struct.unpack("<f", struct.pack("<f", value))[0]
    return 0.0 if rounded == 0.0 else rounded


def factorial(value: int) -> int:
    result = 1
    for factor in range(2, value + 1):
        result *= factor
    return result


def associated_legendre_without_condon_shortley(degree: int, order: int, z: float) -> float:
    if order < 0 or order > degree:
        return 0.0
    pmm = 1.0
    radial = math.sqrt(max(0.0, 1.0 - (z * z)))
    for index in range(1, order + 1):
        pmm *= ((2 * index) - 1) * radial
    if degree == order:
        return pmm
    previous_previous = pmm
    previous = ((2 * order) + 1) * z * pmm
    if degree == order + 1:
        return previous
    for current_degree in range(order + 2, degree + 1):
        current = (((2 * current_degree) - 1) * z * previous
                   - (current_degree + order - 1) * previous_previous) / (current_degree - order)
        previous_previous = previous
        previous = current
    return previous


def sn3d_basis(acn: int, azimuth_degrees: float, elevation_degrees: float) -> float:
    degree = int(math.floor(math.sqrt(acn)))
    signed_order = acn - (degree * (degree + 1))
    absolute_order = abs(signed_order)
    normalization = math.sqrt((1 if signed_order == 0 else 2)
                              * factorial(degree - absolute_order)
                              / factorial(degree + absolute_order))
    azimuth = math.radians(azimuth_degrees)
    elevation = math.radians(elevation_degrees)
    z = math.sin(elevation)
    azimuth_term = (math.sin(absolute_order * azimuth)
                    if signed_order < 0 else math.cos(absolute_order * azimuth))
    return normalization * associated_legendre_without_condon_shortley(
        degree, absolute_order, z) * azimuth_term


def legendre_polynomial(degree: int, x: float) -> float:
    if degree <= 0:
        return 1.0
    if degree == 1:
        return x
    previous_previous = 1.0
    previous = x
    for current_degree in range(2, degree + 1):
        current = (((2 * current_degree) - 1) * x * previous
                   - (current_degree - 1) * previous_previous) / current_degree
        previous_previous = previous
        previous = current
    return previous


def max_re_weight(order: int, degree: int) -> float:
    argument = math.cos(math.radians(137.9) / (order + 1.51))
    return legendre_polynomial(degree, argument)


def fixed_reference_gain(order: int) -> float:
    return {1: 0.50, 2: 0.31, 3: 0.22}[order]


def profile_identity(profile: Profile) -> str:
    speakers = ";".join(
        f"{s.stable_id},{s.role},{s.azimuth:.6f},{s.elevation:.6f},{s.distance:.6f},{s.weight:.6f},1,1"
        for s in profile.speakers
    )
    return (f"speaker-geometry-profile-v1|{profile.profile_id}|1|{profile.dimensionality}"
            f"|unity|{GENERATOR_BACKEND}|{GENERATOR_VERSION}|{speakers}")


def matrix_for(profile: Profile, order: int) -> list[float]:
    source_channels = (order + 1) ** 2
    gain = fixed_reference_gain(order)
    result: list[float] = []
    for output_index, output_speaker in enumerate(profile.speakers):
        for acn in range(source_channels):
            if output_speaker.role == "LFE":
                value = 0.0
            elif output_speaker.role == "M":
                value = gain if acn == 0 else 0.0
            else:
                degree = int(math.floor(math.sqrt(acn)))
                value = (gain * max_re_weight(order, degree)
                         * sn3d_basis(acn, output_speaker.azimuth, output_speaker.elevation))
            result.append(f32(value))
    return result


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest().upper()


def matrix_hash(values: list[float]) -> str:
    return hashlib.sha256(b"".join(struct.pack("<f", value) for value in values)).hexdigest().upper()


def cpp_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def cpp_float(value: float) -> str:
    if value == 0.0:
        return "0.0f"
    literal = f"{value:.9g}"
    if "." not in literal and "e" not in literal.lower():
        literal += ".0"
    return literal + "f"


def render() -> str:
    lines = [
        "// Generated by tools/generate_ambisonic_standard_matrices.py. Do not edit by hand.",
        "#pragma once",
        "",
        "#include <array>",
        "",
        "namespace singleeditor::generated",
        "{",
        "struct StandardSpeakerData",
        "{",
        "    const char* stableId;",
        "    const char* label;",
        "    int role;",
        "    float azimuthDegrees;",
        "    float elevationDegrees;",
        "    float distanceMetres;",
        "    float staticWeight;",
        "    bool enabled;",
        "    bool available;",
        "};",
        "",
        "struct StandardGeometryData",
        "{",
        "    const char* profileId;",
        "    const char* displayName;",
        "    const char* dimensionality;",
        "    const char* profileHash;",
        "    int speakerCount;",
        f"    std::array<StandardSpeakerData, {MAX_OUTPUTS}> speakers;",
        "};",
        "",
        "struct StandardMatrixData",
        "{",
        "    const char* artifactId;",
        "    const char* profileId;",
        "    const char* profileHash;",
        "    const char* matrixHash;",
        "    const char* cacheKey;",
        "    int ambisonicOrder;",
        "    int outputChannels;",
        "    int sourceChannels;",
        f"    std::array<float, {MAX_OUTPUTS * MAX_INPUTS}> gains;",
        "};",
        "",
        f"inline constexpr const char* generatorBackend = {cpp_string(GENERATOR_BACKEND)};",
        f"inline constexpr const char* generatorVersion = {cpp_string(GENERATOR_VERSION)};",
        f"inline constexpr const char* weightProfile = {cpp_string(WEIGHT_PROFILE)};",
        f"inline constexpr const char* staticNormalization = {cpp_string(STATIC_NORMALIZATION)};",
        "",
        f"inline constexpr std::array<StandardGeometryData, {len(PROFILES)}> standardGeometries {{{{",
    ]

    for profile in PROFILES:
        profile_hash = sha256_text(profile_identity(profile))
        lines.extend([
            "    StandardGeometryData {",
            f"        {cpp_string(profile.profile_id)}, {cpp_string(profile.display_name)},",
            f"        {cpp_string(profile.dimensionality)}, {cpp_string(profile_hash)},",
            f"        {len(profile.speakers)}, {{{{",
        ])
        for item in profile.speakers:
            lines.append(
                "            StandardSpeakerData { "
                f"{cpp_string(item.stable_id)}, {cpp_string(item.label)}, {ROLE_VALUES[item.role]}, "
                f"{cpp_float(item.azimuth)}, {cpp_float(item.elevation)}, "
                f"{cpp_float(item.distance)}, {cpp_float(item.weight)}, true, true }},"
            )
        for _ in range(MAX_OUTPUTS - len(profile.speakers)):
            lines.append("            StandardSpeakerData {},")
        lines.extend(["        }}", "    },"])
    lines.extend(["}};", ""])

    matrix_count = len(PROFILES) * 3
    lines.append(f"inline constexpr std::array<StandardMatrixData, {matrix_count}> standardMatrices {{{{")
    for profile in PROFILES:
        profile_hash = sha256_text(profile_identity(profile))
        for order in (1, 2, 3):
            values = matrix_for(profile, order)
            digest = matrix_hash(values)
            cache_identity = (
                f"ambisonic-matrix-cache-v1|ACN|SN3D|order={order}|profile={profile.profile_id}"
                f"|profileHash={profile_hash}|backend={GENERATOR_BACKEND}|version={GENERATOR_VERSION}"
                f"|weight={WEIGHT_PROFILE}|normalization={STATIC_NORMALIZATION}|matrixHash={digest}"
            )
            cache_key = sha256_text(cache_identity)
            artifact_id = f"singleeditor.standard-matrix.v1:{profile.profile_id}:o{order}"
            padded = values + [0.0] * ((MAX_OUTPUTS * MAX_INPUTS) - len(values))
            lines.extend([
                "    StandardMatrixData {",
                f"        {cpp_string(artifact_id)}, {cpp_string(profile.profile_id)},",
                f"        {cpp_string(profile_hash)}, {cpp_string(digest)}, {cpp_string(cache_key)},",
                f"        {order}, {len(profile.speakers)}, {(order + 1) ** 2}, {{{{",
            ])
            for offset in range(0, len(padded), 8):
                lines.append("            " + ", ".join(cpp_float(v) for v in padded[offset:offset + 8]) + ",")
            lines.extend(["        }}", "    },"])
    lines.extend(["}};", "} // namespace singleeditor::generated", ""])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="fail if the checked-in artifact is stale")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    destination = root / "SingleEditorJuce" / "Source" / "Audio" / "AmbisonicStandardMatrixArtifacts.generated.h"
    expected = render()
    if args.check:
        actual = destination.read_text(encoding="utf-8") if destination.exists() else ""
        if actual != expected:
            print(f"stale generated artifact: {destination}")
            return 1
        print(f"artifact up to date: {destination}")
        return 0
    destination.write_text(expected, encoding="utf-8", newline="\n")
    print(f"wrote {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
