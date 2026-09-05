"""Shared helpers for the BookOrbit integration suite.

Two clients live here:

- ``AdminClient`` speaks BookOrbit's own web API (setup, login, koreader
  credentials, book listing) as the seeded test user.
- ``KosyncDevice`` speaks the kosync-compatible device API exactly the way the
  firmware's ``lib/BookOrbitSync/BookOrbitSyncClient.cpp`` does — same
  endpoints, same payload fields — so the seeder can act as a believable
  "other device" without a second simulator.

Everything is stdlib-only so the suite needs no pip install.
"""

from __future__ import annotations

import hashlib
import json
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


# --- .env as the single source of the stack's values -----------------------------

INTEGRATION_DIR = Path(__file__).resolve().parents[1]


def load_env(path: Path = INTEGRATION_DIR / ".env") -> dict[str, str]:
    """Parse the compose .env file so scripts share its values instead of
    duplicating them (KEY=VALUE lines, # comments; no quoting/expansion,
    matching what docker compose does with this file)."""
    values: dict[str, str] = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip()
    return values


ENV = load_env()
BASE_URL = ENV["APP_URL"]
SETUP_TOKEN = ENV["SETUP_BOOTSTRAP_TOKEN"]


# --- KOReader partial MD5 (mirrors lib/KOReaderSync/KOReaderDocumentId.cpp) ---

_CHUNK = 1024


def partial_md5(path: Path) -> str:
    """Content identity BookOrbit keys books on: MD5 over 1KB chunks at
    offset 0 then 1024 << (2*i) for i = 0..10."""
    size = path.stat().st_size
    md5 = hashlib.md5()
    with path.open("rb") as f:
        for i in range(-1, 11):
            offset = 0 if i < 0 else _CHUNK << (2 * i)
            if offset >= size:
                continue
            f.seek(offset)
            chunk = f.read(min(_CHUNK, size - offset))
            if chunk:
                md5.update(chunk)
    return md5.hexdigest()


# --- Tiny HTTP layer -----------------------------------------------------------


class HttpError(RuntimeError):
    def __init__(self, method: str, url: str, status: int, body: str):
        super().__init__(f"{method} {url} -> HTTP {status}: {body[:400]}")
        self.status = status
        self.body = body


def _request(method: str, url: str, headers: dict[str, str], payload: Any | None = None,
             timeout: float = 20.0) -> tuple[int, str]:
    data = None
    if payload is not None:
        data = json.dumps(payload).encode()
        headers = {**headers, "Content-Type": "application/json"}
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")


def request_json(method: str, url: str, headers: dict[str, str], payload: Any | None = None,
                 ok: tuple[int, ...] = (200, 201)) -> Any:
    status, body = _request(method, url, headers, payload)
    if status not in ok:
        raise HttpError(method, url, status, body)
    return json.loads(body) if body.strip() else {}


# --- BookOrbit web API (admin/seeding side) ------------------------------------


@dataclass
class AdminClient:
    base_url: str  # e.g. http://127.0.0.1:3999
    access_token: str = ""

    def _headers(self) -> dict[str, str]:
        headers = {"Accept": "application/json"}
        if self.access_token:
            headers["Authorization"] = f"Bearer {self.access_token}"
        return headers

    def api(self, path: str) -> str:
        return f"{self.base_url}/api/v1{path}"

    def wait_healthy(self, timeout_s: float = 120.0) -> None:
        deadline = time.monotonic() + timeout_s
        while True:
            try:
                status, _ = _request("GET", self.api("/health"), {})
                if status == 200:
                    return
            except OSError:
                pass
            if time.monotonic() > deadline:
                raise RuntimeError("BookOrbit server never became healthy")
            time.sleep(2)

    def setup(self, token: str, username: str, name: str, email: str, password: str) -> bool:
        """First-run bootstrap. Returns False when the server is already set up."""
        status, body = _request("POST", self.api("/auth/setup"),
                                {"Accept": "application/json", "x-setup-token": token},
                                {"username": username, "name": name, "email": email, "password": password})
        if status in (200, 201):
            return True
        # An already-bootstrapped server refuses another setup; that is the
        # idempotent re-run case. A 400 is a real payload problem: surface it.
        if status in (403, 409):
            return False
        raise HttpError("POST", self.api("/auth/setup"), status, body)

    def try_login(self, username: str, password: str) -> bool:
        status, body = _request("POST", self.api("/auth/login"), self._headers(),
                                {"username": username, "password": password})
        if status == 401:
            return False
        if status != 200:
            raise HttpError("POST", self.api("/auth/login"), status, body)
        self.access_token = json.loads(body).get("accessToken", "")
        if not self.access_token:
            raise RuntimeError(f"login returned no accessToken: {body[:300]}")
        return True

    def login(self, username: str, password: str) -> None:
        if not self.try_login(username, password):
            raise RuntimeError("login failed: invalid credentials")

    def ensure_koreader_credentials(self, username: str, password: str) -> None:
        status, body = _request("POST", self.api("/koreader/credentials"), self._headers(),
                                {"username": username, "password": password})
        if status in (200, 201):
            return
        # Already created on a previous seed run: keep it in sync instead.
        request_json("PATCH", self.api("/koreader/credentials"), self._headers(),
                     {"username": username, "password": password}, ok=(200,))

    def ensure_library(self, name: str, folder_path: str) -> tuple[int, int]:
        """Returns (libraryId, folderId), creating the library when none exists."""
        libraries = request_json("GET", self.api("/libraries"), self._headers(), ok=(200,))
        if not libraries:
            request_json("POST", self.api("/libraries"), self._headers(),
                         {"name": name, "icon": "book", "folders": [folder_path]})
            libraries = request_json("GET", self.api("/libraries"), self._headers(), ok=(200,))
        library = libraries[0]
        folders = library.get("folders") or []
        if not folders:
            raise RuntimeError(f"library {library.get('id')} has no folders: {library}")
        folder = folders[0]
        folder_id = folder["id"] if isinstance(folder, dict) else int(folder)
        return int(library["id"]), folder_id

    def dock_pending_count(self) -> int:
        result = request_json("GET", self.api("/book-dock/files?limit=1"), self._headers(), ok=(200,))
        for key in ("total", "totalCount", "count"):
            if isinstance(result, dict) and key in result:
                return int(result[key])
        raise RuntimeError(f"cannot read a dock count out of: {json.dumps(result)[:300]}")

    def dock_finalize_all(self, library_id: int, folder_id: int) -> Any:
        return request_json("POST", self.api("/book-dock/finalize"), self._headers(),
                            {"selectAll": True, "defaultLibraryId": library_id,
                             "defaultFolderId": folder_id})

    def book_count(self) -> int:
        result = request_json("POST", self.api("/books/query"), self._headers(),
                              {"limit": 1, "offset": 0}, ok=(200, 201))
        for key in ("total", "totalCount", "count"):
            if isinstance(result, dict) and key in result:
                return int(result[key])
        if isinstance(result, list):
            return len(result)
        raise RuntimeError(f"cannot read a book count out of: {json.dumps(result)[:300]}")


# --- kosync device API (what the firmware speaks) ------------------------------


@dataclass
class KosyncDevice:
    """A synthetic reader. ``device_id`` mirrors the firmware's
    ``crossink-<mac>`` convention; give each synthetic device its own."""

    base_url: str
    username: str
    password: str
    device_id: str
    device_model: str = "integration-suite"
    plugin_version: str = "crossink-bo-1"

    def _headers(self) -> dict[str, str]:
        return {
            "Accept": "application/json",
            "x-auth-user": self.username,
            "x-auth-key": hashlib.md5(self.password.encode()).hexdigest(),
        }

    def api(self, path: str) -> str:
        return f"{self.base_url}/api/v1/koreader{path}"

    def auth(self) -> None:
        request_json("GET", self.api("/users/auth"), self._headers(), ok=(200,))

    def get_progress(self, document_hash: str) -> dict:
        return request_json("GET", self.api(f"/syncs/progress/{document_hash}"),
                            self._headers(), ok=(200,))

    def put_progress(self, document_hash: str, progress: str, percentage: float,
                     timestamp: int | None = None) -> dict:
        payload: dict[str, Any] = {
            "document": document_hash,
            "progress": progress,  # xpointer, mirrors the firmware
            "percentage": percentage,
            "device": self.device_model,
            "device_id": self.device_id,
        }
        if timestamp:
            payload["timestamp"] = timestamp
        return request_json("PUT", self.api("/syncs/progress"), self._headers(), payload)

    def exchange_annotations(self, document_hash: str, keys: list[dict], keys_complete: bool,
                             changes: list[dict]) -> dict:
        """Mirror of BookOrbitSyncClient::exchangeAnnotations. ``keys`` entries are
        {"k": md5, "dt": datetime}; ``changes`` entries carry datetime/pos0/pos1/text
        (drawer and posFormat are filled in here)."""
        for change in changes:
            change.setdefault("drawer", "lighten")
            change.setdefault("posFormat", "xpointer")
        payload = {
            "deviceId": self.device_id,
            "deviceModel": self.device_model,
            "pluginVersion": self.plugin_version,
            "books": [{
                "hash": document_hash,
                "keysComplete": keys_complete,
                "keys": keys,
                "changes": changes,
            }],
        }
        return request_json("POST", self.api("/plugin/annotations/exchange"),
                            self._headers(), payload)

    def exchange_annotations_ack(self, payload: dict) -> dict:
        return request_json("POST", self.api("/plugin/annotations/exchange-ack"),
                            self._headers(), payload)

    def exchange_bookmarks(self, payload: dict) -> dict:
        return request_json("POST", self.api("/plugin/bookmarks/exchange"),
                            self._headers(), payload)


@dataclass
class SeedManifest:
    """What make_library.py generated and seed.py placed on the server; the
    harness reads this to know which books carry which pre-seeded state."""

    path: Path
    data: dict = field(default_factory=dict)

    @classmethod
    def load(cls, path: Path) -> "SeedManifest":
        return cls(path, json.loads(path.read_text()) if path.exists() else {})

    def save(self) -> None:
        self.path.write_text(json.dumps(self.data, indent=2, sort_keys=True))
