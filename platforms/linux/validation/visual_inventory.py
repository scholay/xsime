#!/usr/bin/env python3
"""Index comparison manifests and optional local assets without granting approval."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inspect_image(manifest, name, repo, require_assets):
    if (not isinstance(name, str) or not name or name in (".", "..")
            or "/" in name or "\\" in name or Path(name).is_absolute()):
        raise ValueError(f"invalid/nonlocal comparison image name in {manifest}: {name!r}")
    image = manifest.parent / name
    if image.is_symlink() or image.resolve().parent != manifest.parent.resolve():
        raise ValueError(f"nonlocal comparison image: {image}")
    result = {"path": str(image.relative_to(repo))}
    if not image.exists():
        if require_assets:
            raise ValueError(f"missing local comparison image: {image}")
        return {**result, "availability": "missing_local"}
    if not image.is_file():
        raise ValueError(f"comparison image is not a regular file: {image}")
    return {**result, "availability": "present_local", "sha256": digest(image)}


def inventory(repo, require_assets=False):
    reports, statuses, assets, pairs = [], Counter(), Counter(), Counter()
    for manifest in sorted((repo / "validation").glob("linux-*/**/manifest.json")):
        if not {"comparison", "compare", "diff"}.intersection(manifest.relative_to(repo).parts):
            continue
        entries = json.loads(manifest.read_text())
        if not isinstance(entries, list) or not entries:
            raise ValueError(f"empty/invalid comparison manifest: {manifest}")
        pages = []
        for row in entries:
            if (not isinstance(row, dict)
                    or any(not isinstance(row.get(key), str) or not row[key]
                           for key in ("page", "status", "pair", "difference_4x"))):
                raise ValueError(f"invalid comparison entry in {manifest}")
            status = row["status"]
            files = {}
            for key in ("pair", "difference_4x"):
                files[key] = inspect_image(manifest, row[key], repo, require_assets)
                assets[files[key]["availability"]] += 1
            present = sum(file["availability"] == "present_local" for file in files.values())
            availability = ("manifest_only", "partial_local_assets", "complete_local_assets")[present]
            pairs[availability] += 1
            pages.append({"page": row["page"], "status": status,
                          "assetAvailability": availability, "files": files})
            statuses[status] += 1
        reports.append({"manifest": str(manifest.relative_to(repo)), "sha256": digest(manifest), "pages": pages})
    return {
        "schemaVersion": 2,
        "scope": "Existing comparison manifests and locally available attachments only; this is not complete state coverage or visual acceptance.",
        "deferredTask": 25,
        "manifestCount": len(reports),
        "pairCount": sum(statuses.values()),
        "statusSource": "Historical comparison manifests; statuses are not revalidated by this inventory.",
        "statusCounts": dict(statuses),
        "assetVerification": {
            "required": require_assets,
            "checks": "Present attachments must be local regular files; SHA-256 is recorded, not compared to a historical digest. Missing attachments are not verified. No pixel or visual review is performed.",
            "presentFileCount": assets["present_local"],
            "missingFileCount": assets["missing_local"],
            "pairCounts": {key: pairs[key] for key in
                           ("manifest_only", "partial_local_assets", "complete_local_assets")},
        },
        "acceptance": "pending: review each discrepancy and cover missing interaction states on the target desktops",
        "reports": reports,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--require-assets", action="store_true",
                        help="Fail if any referenced local comparison attachment is missing.")
    args = parser.parse_args()
    result = inventory(args.repo.resolve(), args.require_assets)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    assets = result["assetVerification"]
    print(f"{result['manifestCount']} comparison manifests; {result['pairCount']} recorded pairs; "
          f"local attachments: {assets['presentFileCount']} checked and hashed, "
          f"{assets['missingFileCount']} missing (not verified); "
          f"historical statuses={result['statusCounts']}; visual acceptance remains pending")


if __name__ == "__main__":
    main()
