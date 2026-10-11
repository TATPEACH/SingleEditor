#!/usr/bin/env python3
"""Generate Single Editor's compact ACN/SN3D binaural FIR resource."""

from __future__ import annotations

import argparse
import hashlib
import math
import struct
import zlib
from pathlib import Path

import numpy as np


MAGIC = b"SEHRIR1\0"
VERSION = 1
HEADER = struct.Struct("<8sIIII32sI")
BANK_HEADER = struct.Struct("<II")
EXPECTED_SOURCE_MD5 = "05aaa22e2d5654cbdee5cbe83ebfb1ef"


def fibonacci_directions(count: int) -> np.ndarray:
    indices = np.arange(count, dtype=np.float64)
    z = 1.0 - (2.0 * (indices + 0.5) / count)
    azimuth = indices * (math.pi * (3.0 - math.sqrt(5.0)))
    radius = np.sqrt(np.maximum(0.0, 1.0 - z * z))
    return np.column_stack((radius * np.cos(azimuth), radius * np.sin(azimuth), z))


def _associated_legendre_without_condon_shortley(degree: int, order: int, z: np.ndarray) -> np.ndarray:
    pmm = np.ones_like(z)
    radial = np.sqrt(np.maximum(0.0, 1.0 - z * z))
    for index in range(1, order + 1):
        pmm *= ((2 * index) - 1) * radial
    if degree == order:
        return pmm

    previous_previous = pmm
    previous = ((2 * order) + 1) * z * pmm
    if degree == order + 1:
        return previous

    for current_degree in range(order + 2, degree + 1):
        current = (
            ((2 * current_degree) - 1) * z * previous
            - (current_degree + order - 1) * previous_previous
        ) / (current_degree - order)
        previous_previous = previous
        previous = current
    return previous


def sn3d_basis(directions: np.ndarray, order: int) -> np.ndarray:
    directions = np.asarray(directions, dtype=np.float64)
    if directions.ndim != 2 or directions.shape[1] != 3:
        raise ValueError("directions must have shape (count, 3)")
    if order not in (1, 2, 3):
        raise ValueError("only first through third order are supported")

    lengths = np.linalg.norm(directions, axis=1)
    if np.any(lengths <= 0.0):
        raise ValueError("directions must be non-zero")
    unit = directions / lengths[:, np.newaxis]
    azimuth = np.arctan2(unit[:, 1], unit[:, 0])
    basis = np.empty((len(unit), (order + 1) ** 2), dtype=np.float64)

    for acn in range(basis.shape[1]):
        degree = math.isqrt(acn)
        signed_order = acn - (degree * (degree + 1))
        absolute_order = abs(signed_order)
        normalization = math.sqrt(
            (1 if signed_order == 0 else 2)
            * math.factorial(degree - absolute_order)
            / math.factorial(degree + absolute_order)
        )
        azimuth_term = (
            np.sin(absolute_order * azimuth)
            if signed_order < 0
            else np.cos(absolute_order * azimuth)
        )
        basis[:, acn] = (
            normalization
            * _associated_legendre_without_condon_shortley(degree, absolute_order, unit[:, 2])
            * azimuth_term
        )

    return basis


def fit_filter_bank(directions: np.ndarray, measured_hrirs: np.ndarray, order: int) -> np.ndarray:
    hrirs = np.asarray(measured_hrirs, dtype=np.float64)
    if hrirs.ndim != 3 or hrirs.shape[0] != len(directions) or hrirs.shape[1] != 2:
        raise ValueError("measured_hrirs must have shape (directions, 2, taps)")
    basis = sn3d_basis(directions, order)
    coefficients, _, _, _ = np.linalg.lstsq(basis, hrirs.reshape(len(directions), -1), rcond=None)
    return coefficients.reshape(basis.shape[1], 2, hrirs.shape[2]).transpose(1, 0, 2)


def write_resource(
    output: Path,
    banks: dict[int, np.ndarray],
    sample_rate: int,
    source_md5: str,
) -> None:
    if sorted(banks) != [1, 2, 3]:
        raise ValueError("resource must contain first, second, and third-order banks")
    tap_counts = {np.asarray(bank).shape[2] for bank in banks.values()}
    if len(tap_counts) != 1:
        raise ValueError("all banks must use the same tap count")
    if len(source_md5) != 32:
        raise ValueError("source_md5 must contain 32 hexadecimal characters")

    payload = bytearray()
    for order in (1, 2, 3):
        bank = np.asarray(banks[order], dtype="<f4")
        expected_channels = (order + 1) ** 2
        if bank.shape[:2] != (2, expected_channels):
            raise ValueError(f"order {order} bank must have shape (2, {expected_channels}, taps)")
        payload += BANK_HEADER.pack(order, expected_channels)
        payload += bank.tobytes(order="C")

    header = HEADER.pack(
        MAGIC,
        VERSION,
        int(sample_rate),
        tap_counts.pop(),
        3,
        source_md5.lower().encode("ascii"),
        zlib.crc32(payload) & 0xFFFFFFFF,
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(header + payload)


def read_resource(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < HEADER.size:
        raise ValueError("resource is truncated")
    magic, version, sample_rate, tap_count, bank_count, source_md5, checksum = HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION or bank_count != 3:
        raise ValueError("resource header is unsupported")
    payload = data[HEADER.size:]
    if zlib.crc32(payload) & 0xFFFFFFFF != checksum:
        raise ValueError("resource checksum does not match")

    offset = 0
    banks = {}
    for _ in range(bank_count):
        order, channels = BANK_HEADER.unpack_from(payload, offset)
        offset += BANK_HEADER.size
        sample_count = 2 * channels * tap_count
        byte_count = sample_count * 4
        bank = np.frombuffer(payload, dtype="<f4", count=sample_count, offset=offset)
        banks[order] = bank.copy().reshape(2, channels, tap_count)
        offset += byte_count
    if offset != len(payload):
        raise ValueError("resource contains unexpected trailing data")

    return {
        "version": version,
        "sample_rate": sample_rate,
        "tap_count": tap_count,
        "source_md5": source_md5.decode("ascii"),
        "banks": banks,
    }


def load_sofa(path: Path) -> tuple[np.ndarray, np.ndarray, int]:
    try:
        import h5py
    except ImportError as error:
        raise RuntimeError("h5py is required only when regenerating the resource") from error

    if hashlib.md5(path.read_bytes()).hexdigest() != EXPECTED_SOURCE_MD5:
        raise ValueError("SOFA source MD5 does not match the pinned SADIE_006 file")
    with h5py.File(path, "r") as sofa:
        positions = np.asarray(sofa["SourcePosition"], dtype=np.float64)
        hrirs = np.asarray(sofa["Data.IR"], dtype=np.float64)
        sample_rate = int(round(float(np.asarray(sofa["Data.SamplingRate"])[0])))

    azimuth = np.deg2rad(positions[:, 0])
    elevation = np.deg2rad(positions[:, 1])
    elevation_cosine = np.cos(elevation)
    directions = np.column_stack(
        (elevation_cosine * np.cos(azimuth), elevation_cosine * np.sin(azimuth), np.sin(elevation))
    )
    return directions, hrirs, sample_rate


def generate(input_sofa: Path, output: Path) -> None:
    directions, hrirs, sample_rate = load_sofa(input_sofa)
    banks = {order: fit_filter_bank(directions, hrirs, order).astype(np.float32) for order in (1, 2, 3)}
    write_resource(output, banks, sample_rate, EXPECTED_SOURCE_MD5)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_sofa", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    generate(args.input_sofa, args.output)
    parsed = read_resource(args.output)
    print(
        f"wrote {args.output} ({args.output.stat().st_size} bytes, "
        f"{parsed['sample_rate']} Hz, {parsed['tap_count']} taps)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
