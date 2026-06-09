#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0

"""Generate an OXRSys webcam rig calibration JSON from paired checkerboard images."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

cv2 = None
np = None


IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".tif", ".tiff", ".bmp"}


def load_optional_dependencies() -> bool:
    global cv2
    global np
    try:
        import cv2 as cv2_module
        import numpy as numpy_module
    except ImportError:
        return False
    cv2 = cv2_module
    np = numpy_module
    return True


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Calibrate a two-camera macOS webcam rig from paired checkerboard images. "
            "Device IDs must match AVFoundation uniqueID values used by the simulator."
        )
    )
    parser.add_argument(
        "--camera",
        action="append",
        required=True,
        metavar="DEVICE_ID=IMAGE_DIR",
        help="camera device ID and image directory; pass exactly two cameras",
    )
    parser.add_argument(
        "--pattern-size",
        default="9x6",
        help="inner checkerboard corners as COLSxROWS, default: 9x6",
    )
    parser.add_argument(
        "--square-size",
        required=True,
        type=float,
        help="checkerboard square size in meters",
    )
    parser.add_argument("--output", required=True, type=Path, help="output JSON path")
    return parser.parse_args()


def parse_pattern_size(value: str) -> tuple[int, int]:
    parts = value.lower().split("x")
    if len(parts) != 2:
        raise ValueError("--pattern-size must use COLSxROWS")
    cols, rows = int(parts[0]), int(parts[1])
    if cols < 2 or rows < 2:
        raise ValueError("--pattern-size must contain at least 2x2 inner corners")
    return cols, rows


def parse_camera_specs(values: list[str]) -> list[tuple[str, Path]]:
    cameras = []
    for value in values:
        if "=" not in value:
            raise ValueError("--camera must use DEVICE_ID=IMAGE_DIR")
        device_id, directory = value.split("=", 1)
        if not device_id:
            raise ValueError("camera device ID cannot be empty")
        image_dir = Path(directory).expanduser()
        if not image_dir.is_dir():
            raise ValueError(f"{image_dir} is not a directory")
        cameras.append((device_id, image_dir))
    if len(cameras) != 2:
        raise ValueError("this calibration tool expects exactly two cameras")
    return cameras


def image_files(directory: Path) -> dict[str, Path]:
    return {
        path.stem: path
        for path in sorted(directory.iterdir())
        if path.suffix.lower() in IMAGE_EXTENSIONS and path.is_file()
    }


def paired_images(first: Path, second: Path) -> list[tuple[Path, Path]]:
    first_images = image_files(first)
    second_images = image_files(second)
    stems = sorted(set(first_images) & set(second_images))
    return [(first_images[stem], second_images[stem]) for stem in stems]


def object_points(pattern_size: tuple[int, int], square_size: float) -> np.ndarray:
    cols, rows = pattern_size
    points = np.zeros((cols * rows, 3), np.float32)
    points[:, :2] = np.mgrid[0:cols, 0:rows].T.reshape(-1, 2)
    points *= square_size
    return points


def find_corners(path: Path, pattern_size: tuple[int, int]):
    image = cv2.imread(str(path))
    if image is None:
        return None, None
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    found, corners = cv2.findChessboardCorners(gray, pattern_size, None)
    if not found:
        return None, gray.shape[::-1]
    criteria = (
        cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER,
        30,
        0.001,
    )
    corners = cv2.cornerSubPix(gray, corners, (11, 11), (-1, -1), criteria)
    return corners, gray.shape[::-1]


def row_major(matrix: np.ndarray) -> list[float]:
    return [float(value) for value in matrix.reshape(-1)]


def vector_values(vector: np.ndarray) -> list[float]:
    return [float(value) for value in vector.reshape(-1)]


def swift_camera_transform_from_opencv(
    rotation_cam1_to_cam2: np.ndarray,
    translation_cam1_to_cam2: np.ndarray,
):
    convert = np.diag([1.0, -1.0, -1.0])
    rotation_cam2_to_cam1 = rotation_cam1_to_cam2.T
    swift_rotation = convert @ rotation_cam2_to_cam1 @ convert
    swift_translation = -convert @ rotation_cam2_to_cam1 @ translation_cam1_to_cam2
    return swift_rotation, swift_translation


def main() -> int:
    try:
        args = parse_args()
        pattern_size = parse_pattern_size(args.pattern_size)
        cameras = parse_camera_specs(args.camera)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if not load_optional_dependencies():
        print("OpenCV and NumPy are required: python3 -m pip install opencv-python", file=sys.stderr)
        return 2

    pairs = paired_images(cameras[0][1], cameras[1][1])
    if not pairs:
        print("No paired images found; use matching file stems in both directories", file=sys.stderr)
        return 2

    base_object_points = object_points(pattern_size, args.square_size)
    all_object_points = []
    image_points = [[], []]
    image_size = None

    for first_path, second_path in pairs:
        first_corners, first_size = find_corners(first_path, pattern_size)
        second_corners, second_size = find_corners(second_path, pattern_size)
        if first_corners is None or second_corners is None:
            continue
        if first_size != second_size:
            print(f"Skipping size mismatch: {first_path.name}", file=sys.stderr)
            continue
        image_size = first_size
        all_object_points.append(base_object_points)
        image_points[0].append(first_corners)
        image_points[1].append(second_corners)

    if len(all_object_points) < 5:
        print("Need at least five valid paired checkerboard detections", file=sys.stderr)
        return 2

    _, camera_matrix_1, dist_1, _, _ = cv2.calibrateCamera(
        all_object_points,
        image_points[0],
        image_size,
        None,
        None,
    )
    _, camera_matrix_2, dist_2, _, _ = cv2.calibrateCamera(
        all_object_points,
        image_points[1],
        image_size,
        None,
        None,
    )
    reprojection_error, camera_matrix_1, dist_1, camera_matrix_2, dist_2, rotation, translation, _, _ = (
        cv2.stereoCalibrate(
            all_object_points,
            image_points[0],
            image_points[1],
            camera_matrix_1,
            dist_1,
            camera_matrix_2,
            dist_2,
            image_size,
            flags=cv2.CALIB_FIX_INTRINSIC,
        )
    )

    second_rotation, second_translation = swift_camera_transform_from_opencv(
        rotation,
        translation,
    )
    width, height = image_size
    output = {
        "reprojectionError": float(reprojection_error),
        "cameras": [
            {
                "deviceID": cameras[0][0],
                "imageWidth": float(width),
                "imageHeight": float(height),
                "fx": float(camera_matrix_1[0, 0]),
                "fy": float(camera_matrix_1[1, 1]),
                "cx": float(camera_matrix_1[0, 2]),
                "cy": float(camera_matrix_1[1, 2]),
                "cameraToTrackingRotation": row_major(np.eye(3)),
                "cameraToTrackingTranslation": [0.0, 0.0, 0.0],
            },
            {
                "deviceID": cameras[1][0],
                "imageWidth": float(width),
                "imageHeight": float(height),
                "fx": float(camera_matrix_2[0, 0]),
                "fy": float(camera_matrix_2[1, 1]),
                "cx": float(camera_matrix_2[0, 2]),
                "cy": float(camera_matrix_2[1, 2]),
                "cameraToTrackingRotation": row_major(second_rotation),
                "cameraToTrackingTranslation": vector_values(second_translation),
            },
        ],
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {args.output} from {len(all_object_points)} paired frames")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
