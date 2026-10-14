#!/usr/bin/env python3
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Generate a PDF graph from an MLIR file.

The generated graph uses:
  - circles to represent operations
  - lines to represent producer/consumer relations based on SSA values
"""

from __future__ import annotations

import argparse
import math
import pathlib
import re
from dataclasses import dataclass


VALUE_PATTERN = re.compile(r"%[-A-Za-z0-9_.$]+(?:#\d+)?")
RESULT_OP_PATTERN = re.compile(
    r'^\s*(?P<results>(?:%[-A-Za-z0-9_.$]+(?::\d+)?(?:#\d+)?\s*,\s*)*'
    r'%[-A-Za-z0-9_.$]+(?::\d+)?(?:#\d+)?)\s*=\s*'
    r'(?P<op>"[^"]+"|[-A-Za-z0-9_.$]+)\b(?P<rest>.*)$'
)
OP_ONLY_PATTERN = re.compile(
    r'^\s*(?P<op>"[^"]+"|[-A-Za-z0-9_.$]+)\b(?P<rest>.*)$'
)


@dataclass(frozen=True)
class Operation:
    node_id: str
    name: str
    line_no: int


def normalize_operation_name(raw_name: str) -> str:
    if raw_name.startswith('"') and raw_name.endswith('"'):
        return raw_name[1:-1]
    return raw_name


def parse_result_names(results_text: str) -> list[str]:
    values: list[str] = []
    for item in results_text.split(","):
        token = item.strip()
        if not token:
            continue
        expanded = re.fullmatch(r"(%[-A-Za-z0-9_.$]+):(\d+)", token)
        if expanded:
            base_name = expanded.group(1)
            count = int(expanded.group(2))
            for index in range(count):
                values.append(f"{base_name}#{index}")
            continue
        values.append(token)
    return values


def parse_operation_line(line: str) -> tuple[list[str], str, str] | None:
    match = RESULT_OP_PATTERN.match(line)
    if match:
        return (
            parse_result_names(match.group("results")),
            normalize_operation_name(match.group("op")),
            match.group("rest"),
        )

    match = OP_ONLY_PATTERN.match(line)
    if not match:
        return None
    return (
        [],
        normalize_operation_name(match.group("op")),
        match.group("rest"),
    )


def parse_mlir_graph(mlir_text: str) -> tuple[list[Operation], set[tuple[str, str]]]:
    operations: list[Operation] = []
    edges: set[tuple[str, str]] = set()
    value_to_producer: dict[str, str] = {}

    for line_no, raw_line in enumerate(mlir_text.splitlines(), start=1):
        line = raw_line.split("//", maxsplit=1)[0].strip()
        if not line:
            continue
        if line[0] in "{}^#!":
            continue
        if line.startswith("]") or line.startswith(")"):
            continue

        parsed = parse_operation_line(line)
        if not parsed:
            continue
        result_values, operation_name, rest = parsed

        node_id = f"op{len(operations)}"
        operation = Operation(node_id=node_id, name=operation_name, line_no=line_no)
        operations.append(operation)

        for used_value in VALUE_PATTERN.findall(rest):
            producer = value_to_producer.get(used_value)
            if producer is not None and producer != node_id:
                edges.add((producer, node_id))

        for result_value in result_values:
            value_to_producer[result_value] = node_id

    return operations, edges


def escape_pdf_text(text: str) -> str:
    return text.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


def format_node_label(operation_name: str, max_length: int = 14) -> str:
    if len(operation_name) <= max_length:
        return operation_name
    return operation_name[: max_length - 3] + "..."


def draw_circle_commands(x: float, y: float, radius: float) -> list[str]:
    kappa = 0.552284749831 * radius
    return [
        f"{x + radius:.2f} {y:.2f} m",
        (
            f"{x + radius:.2f} {y + kappa:.2f} "
            f"{x + kappa:.2f} {y + radius:.2f} "
            f"{x:.2f} {y + radius:.2f} c"
        ),
        (
            f"{x - kappa:.2f} {y + radius:.2f} "
            f"{x - radius:.2f} {y + kappa:.2f} "
            f"{x - radius:.2f} {y:.2f} c"
        ),
        (
            f"{x - radius:.2f} {y - kappa:.2f} "
            f"{x - kappa:.2f} {y - radius:.2f} "
            f"{x:.2f} {y - radius:.2f} c"
        ),
        (
            f"{x + kappa:.2f} {y - radius:.2f} "
            f"{x + radius:.2f} {y - kappa:.2f} "
            f"{x + radius:.2f} {y:.2f} c"
        ),
        "S",
    ]


def circle_layout(count: int, center_x: float, center_y: float, graph_radius: float) -> list[tuple[float, float]]:
    if count == 0:
        return []
    if count == 1:
        return [(center_x, center_y)]

    points: list[tuple[float, float]] = []
    for index in range(count):
        angle = (2.0 * math.pi * index / count) - (math.pi / 2.0)
        x = center_x + graph_radius * math.cos(angle)
        y = center_y + graph_radius * math.sin(angle)
        points.append((x, y))
    return points


def write_pdf_graph(
    output_path: pathlib.Path, operations: list[Operation], edges: set[tuple[str, str]]
) -> None:
    margin = 40.0
    graph_panel = 640.0
    legend_panel = 300.0
    line_height = 13.0
    min_height = graph_panel + 2.0 * margin
    legend_height = max(200.0, (len(operations) + 3) * line_height + 2.0 * margin)
    page_width = graph_panel + legend_panel + 2.0 * margin
    page_height = max(min_height, legend_height)

    node_labels = {
        operation.node_id: format_node_label(operation.name) for operation in operations
    }
    max_label_length = max((len(label) for label in node_labels.values()), default=1)

    center_x = margin + graph_panel / 2.0
    center_y = page_height / 2.0
    node_radius = max(20.0, min(40.0, 10.0 + max_label_length * 1.2))
    max_graph_radius = graph_panel / 2.0 - node_radius - 20.0
    required_spacing = 2.6 * node_radius
    required_graph_radius = (required_spacing * max(1, len(operations))) / (2.0 * math.pi)
    graph_radius = max(
        40.0,
        min(
            max_graph_radius,
            max(required_graph_radius, 36.0 + len(operations) * 6.0),
        ),
    )

    node_positions: dict[str, tuple[float, float]] = {}
    for operation, position in zip(
        operations, circle_layout(len(operations), center_x, center_y, graph_radius)
    ):
        node_positions[operation.node_id] = position

    commands: list[str] = []
    commands.append("0 G 1 w")

    for source_id, destination_id in sorted(edges):
        if source_id not in node_positions or destination_id not in node_positions:
            continue
        source_x, source_y = node_positions[source_id]
        destination_x, destination_y = node_positions[destination_id]
        delta_x = destination_x - source_x
        delta_y = destination_y - source_y
        distance = math.hypot(delta_x, delta_y)
        if distance <= 0.01:
            continue
        offset_x = (delta_x / distance) * node_radius
        offset_y = (delta_y / distance) * node_radius
        start_x = source_x + offset_x
        start_y = source_y + offset_y
        end_x = destination_x - offset_x
        end_y = destination_y - offset_y
        commands.append(f"{start_x:.2f} {start_y:.2f} m {end_x:.2f} {end_y:.2f} l S")

    for index, operation in enumerate(operations):
        node_x, node_y = node_positions[operation.node_id]
        commands.extend(draw_circle_commands(node_x, node_y, node_radius))
        label = node_labels[operation.node_id]
        label_font_size = max(6.0, min(9.0, node_radius / 2.8))
        label_width = len(label) * label_font_size * 0.5
        text_x = node_x - (label_width / 2.0)
        text_y = node_y - (label_font_size * 0.33)
        commands.append(
            f"BT /F1 {label_font_size:.2f} Tf {text_x:.2f} {text_y:.2f} Td "
            f"({escape_pdf_text(label)}) Tj ET"
        )

    legend_x = margin + graph_panel + 15.0
    legend_y = page_height - margin - 8.0
    commands.append(f"BT /F1 11 Tf {legend_x:.2f} {legend_y:.2f} Td (Legend) Tj ET")
    legend_y -= line_height
    commands.append(
        f"BT /F1 9 Tf {legend_x:.2f} {legend_y:.2f} Td (id: operation @ line) Tj ET"
    )

    for index, operation in enumerate(operations):
        legend_y -= line_height
        if legend_y < margin:
            break
        operation_name = operation.name
        if len(operation_name) > 34:
            operation_name = operation_name[:31] + "..."
        text = f"{index}: {operation_name} @ L{operation.line_no}"
        commands.append(
            f"BT /F1 8 Tf {legend_x:.2f} {legend_y:.2f} Td ({escape_pdf_text(text)}) Tj ET"
        )

    if legend_y < margin:
        truncated_note = "(Legend truncated: too many operations)"
        commands.append(
            f"BT /F1 8 Tf {legend_x:.2f} {margin:.2f} Td {truncated_note} Tj ET"
        )

    content_stream = "\n".join(commands).encode("utf-8")
    objects: list[bytes] = []
    objects.append(b"<< /Type /Catalog /Pages 2 0 R >>")
    objects.append(b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    page_object = (
        f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {page_width:.2f} {page_height:.2f}] "
        f"/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>"
    )
    objects.append(page_object.encode("utf-8"))
    objects.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    objects.append(
        b"<< /Length " + str(len(content_stream)).encode("ascii") + b" >>\nstream\n"
        + content_stream
        + b"\nendstream"
    )

    pdf_data = bytearray()
    pdf_data.extend(b"%PDF-1.4\n")
    offsets = [0]
    for index, obj in enumerate(objects, start=1):
        offsets.append(len(pdf_data))
        pdf_data.extend(f"{index} 0 obj\n".encode("ascii"))
        pdf_data.extend(obj)
        pdf_data.extend(b"\nendobj\n")

    xref_offset = len(pdf_data)
    pdf_data.extend(f"xref\n0 {len(objects) + 1}\n".encode("ascii"))
    pdf_data.extend(b"0000000000 65535 f \n")
    for offset in offsets[1:]:
        pdf_data.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
    pdf_data.extend(
        (
            f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\n"
            f"startxref\n{xref_offset}\n%%EOF\n"
        ).encode("ascii")
    )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(pdf_data)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Generate a PDF operation graph from an MLIR file. "
            "Nodes are circles and edges represent SSA dependencies."
        )
    )
    parser.add_argument("mlir_file", type=pathlib.Path, help="Input MLIR file path")
    parser.add_argument(
        "-o",
        "--output",
        type=pathlib.Path,
        help="Output PDF path (default: <input_stem>_op_graph.pdf)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    mlir_path = args.mlir_file
    if not mlir_path.exists():
        raise FileNotFoundError(f"Input MLIR file does not exist: {mlir_path}")

    output_path = args.output
    if output_path is None:
        output_path = mlir_path.with_name(f"{mlir_path.stem}_op_graph.pdf")

    mlir_text = mlir_path.read_text(encoding="utf-8")
    operations, edges = parse_mlir_graph(mlir_text)
    write_pdf_graph(output_path, operations, edges)

    print(
        f"Generated {output_path} with {len(operations)} operations and "
        f"{len(edges)} relations."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
