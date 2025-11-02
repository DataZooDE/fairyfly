#!/usr/bin/env python3
"""Analyze fairyfly JSON output to find where text content is hiding"""

import json
import sys

def analyze_element(elem, path="root", depth=0):
    """Recursively analyze an element and its children"""
    elem_type = elem.get("type", "unknown")
    elem_id = elem.get("id", "N/A")
    text = elem.get("text", "")
    subtype = elem.get("subtype", "")

    # Print elements with text content
    if len(text) > 20:
        print(f"{'  ' * depth}[{path}] {elem_type}", end="")
        if subtype:
            print(f" (SubType: {subtype})", end="")
        print(f" - {len(text)} chars")
        print(f"{'  ' * depth}  ID: {elem_id}")
        print(f"{'  ' * depth}  Text preview: {text[:100]}...")
        print()

    # Recurse into children
    if "children" in elem and isinstance(elem["children"], list):
        for i, child in enumerate(elem["children"]):
            analyze_element(child, f"{path}/child[{i}]", depth + 1)

def main():
    # Read JSON from stdin or file
    if len(sys.argv) > 1:
        with open(sys.argv[1], 'r', encoding='utf-8') as f:
            data = json.load(f)
    else:
        data = json.load(sys.stdin)

    print("=== FAIRYFLY JSON STRUCTURE ANALYSIS ===")
    print()

    elements = data.get("data", {}).get("elements", [])
    hierarchy = data.get("data", {}).get("hierarchy", {})

    print(f"Total top-level elements: {len(elements)}")
    print()

    # Count element types in hierarchy
    print("Hierarchy categories:")
    for category in ["buttons", "form_fields", "tabs", "other"]:
        count = len(hierarchy.get(category, []))
        print(f"  {category}: {count} elements")
    print()

    # Analyze elements array
    print("=== Elements with text content (from 'elements' array): ===")
    for i, elem in enumerate(elements):
        analyze_element(elem, f"elements[{i}]", 0)

    # Analyze hierarchy.other
    print("=== Elements with text content (from 'hierarchy.other'): ===")
    for i, elem in enumerate(hierarchy.get("other", [])):
        analyze_element(elem, f"hierarchy.other[{i}]", 0)

    # Count element types
    print("=== Element type counts: ===")
    type_counts = {}

    def count_types(elem):
        elem_type = elem.get("type", "unknown")
        type_counts[elem_type] = type_counts.get(elem_type, 0) + 1
        if "children" in elem:
            for child in elem["children"]:
                count_types(child)

    for elem in elements:
        count_types(elem)

    for elem_type in sorted(type_counts.keys()):
        print(f"  {elem_type}: {type_counts[elem_type]}")

if __name__ == "__main__":
    main()
