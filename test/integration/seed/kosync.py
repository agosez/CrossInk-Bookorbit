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


def kodatetime(epoch: int) -> str:
    """KOReader's annotation datetime format, what the exchange endpoints validate."""
    return time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(epoch))


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


def upload_file(url: str, headers: dict[str, str], path: Path,
                ok: tuple[int, ...] = (200, 201)) -> tuple[int, Any]:
    """POST ``path`` as the single ``file`` part of a multipart form. Returns the
    status and parsed body; a status outside ``ok`` raises unless it is a 409,
    which the seeder reads as "already there"."""
    boundary = f"crossink-{int(time.time() * 1000)}"
    body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; "
            f"filename=\"{path.name}\"\r\nContent-Type: application/octet-stream\r\n\r\n").encode()
    body += path.read_bytes() + f"\r\n--{boundary}--\r\n".encode()
    req = urllib.request.Request(url, data=body, method="POST", headers={
        **headers, "Content-Type": f"multipart/form-data; boundary={boundary}"})
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            status, text = resp.status, resp.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        status, text = e.code, e.read().decode(errors="replace")
    if status not in ok and status != 409:
        raise HttpError("POST", url, status, text)
    return status, json.loads(text) if text.strip() else {}


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

    def find_book_ids(self, titles: list[str]) -> dict[str, int]:
        """Map book titles to their server-side book ids. Books whose metadata
        scan has not run yet are titled by filename stem, so callers may pass
        either form; keys of the result are the matched server titles."""
        wanted = set(titles)
        found: dict[str, int] = {}
        page = 0
        while True:
            result = request_json("POST", self.api("/books/query"), self._headers(),
                                  {"pagination": {"page": page, "size": 200}}, ok=(200, 201))
            items = result.get("items", [])
            for item in items:
                if item.get("title") in wanted:
                    found[item["title"]] = int(item["id"])
            page += 1
            if len(found) >= len(wanted) or page * 200 >= int(result.get("total", 0)) or not items:
                break
        return found

    def set_file_naming_pattern(self, pattern: str) -> None:
        """Set the account's KOReader file naming template — the one BookOrbit applies
        to devices without an override, and which the catalog resolves per file."""
        request_json("PUT", self.api("/koreader/file-naming-pattern"), self._headers(),
                     {"pattern": pattern}, ok=(200,))

    def ensure_collection(self, name: str, icon: str, book_ids: list[int]) -> int:
        """Create a collection holding ``book_ids``, or return the existing one by name."""
        for collection in request_json("GET", self.api("/collections"), self._headers(), ok=(200,)):
            if collection.get("name") == name:
                return int(collection["id"])
        created = request_json("POST", self.api("/collections"), self._headers(),
                               {"name": name, "icon": icon})
        collection_id = int(created["id"])
        if book_ids:  # the bulk-selection DTO rejects an empty bookIds list
            request_json("POST", self.api(f"/collections/{collection_id}/books"),
                         self._headers(), {"bookIds": book_ids})
        return collection_id

    def upload_book(self, library_id: int, path: Path) -> int | None:
        """Upload ``path`` as a new book of the library. Returns its book id, or None
        when a file of that name is already there (409)."""
        status, body = upload_file(self.api(f"/libraries/{library_id}/upload"), self._headers(), path)
        return None if status == 409 else int(body["bookId"])

    def add_book_file(self, book_id: int, path: Path) -> None:
        """Attach ``path`` to an existing book as another format; already attached
        (409) is fine."""
        upload_file(self.api(f"/books/{book_id}/files"), self._headers(), path)

    def ensure_smart_scope(self, name: str, icon: str, rules: list[dict]) -> int:
        """Create a SmartScope matching ``rules``, or return the existing one by name.

        ``rules`` are the server's filter rules, ANDed together. A saved scope is
        re-filtered on every query, so the catalog's smart-scopes section is a live
        view rather than a fixed member list like a collection's.
        """
        for scope in request_json("GET", self.api("/smart-scopes"), self._headers(), ok=(200,)):
            if scope.get("name") == name:
                return int(scope["id"])
        created = request_json("POST", self.api("/smart-scopes"), self._headers(),
                               {"name": name, "icon": icon, "defaultSort": [],
                                "filter": {"type": "group", "join": "AND", "rules": rules}})
        return int(created["id"])


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

    def catalog_books(self, sort: str, page: int = 1, size: int = 20) -> dict:
        """One page of the catalog book listing, exactly as the firmware asks for it."""
        return request_json("GET", self.api(f"/plugin/catalog/books?page={page}&size={size}&sort={sort}"),
                            self._headers(), ok=(200,))

    def catalog_page(self, **params: Any) -> dict:
        """One page of the catalog book listing with arbitrary query parameters —
        the server's own answer to compare the firmware's listings against."""
        query = "&".join(f"{k}={v}" for k, v in {"page": 1, "size": 20, **params}.items())
        return request_json("GET", self.api(f"/plugin/catalog/books?{query}"), self._headers(), ok=(200,))

    def set_read_status(self, book_id: int, status: str) -> None:
        request_json("PUT", self.api(f"/plugin/catalog/books/{book_id}/read-status"), self._headers(),
                     {"status": status}, ok=(200,))

    def catalog_book_detail(self, book_id: int) -> dict:
        """A catalog book's detail, including each file's devicePath — the account's
        naming template resolved server-side for this device."""
        return request_json("GET", self.api(f"/plugin/catalog/books/{book_id}?deviceId={self.device_id}"),
                            self._headers(), ok=(200,))

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

    def exchange_bookmarks(self, document_hash: str, keys: list[dict], keys_complete: bool,
                           changes: list[dict]) -> dict:
        """Mirror of BookOrbitSyncClient::exchangeBookmarks. ``changes`` entries carry
        datetime/pos only — the bookmark DTO rejects posFormat."""
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
        return request_json("POST", self.api("/plugin/bookmarks/exchange"),
                            self._headers(), payload)

    def exchange_bookmarks_ack(self, document_hash: str, applied: list[dict] = (),
                               deleted: list[dict] = ()) -> dict:
        """``applied`` entries are {serverId, status, key|datetime+pos}; ``deleted``
        entries are {serverId, status}. status is 'applied' or 'failed'."""
        payload = {
            "deviceId": self.device_id,
            "deviceModel": self.device_model,
            "pluginVersion": self.plugin_version,
            "books": [{
                "hash": document_hash,
                "applied": list(applied),
                "deleted": list(deleted),
            }],
        }
        return request_json("POST", self.api("/plugin/bookmarks/exchange-ack"),
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
