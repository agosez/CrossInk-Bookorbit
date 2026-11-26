"""Reading position: the Smart sync started from the home screen, and Sync on Sleep, which
syncs from behind the sleep screen and leaves to the user what it cannot settle alone."""

from __future__ import annotations

import time

from formats import read_progress_bin, write_progress_bin
from server import device, load_seed, open_in_web_reader, peer_device, web_session
from sim import LIBRARY, SIM_DEVICE_ID, run_simulator, sd_card, sleep_sync, sync


def scenario_sync_progress_pull() -> None:
    """A further server-side position (seeded by the synthetic peer) reaches the
    simulator through a Smart sync triggered from the home screen, and lands in both
    of its local progress homes."""
    manifest, _ = load_seed()
    seeded = manifest["progress"][0]

    with sd_card(manifest) as fs:
        # Local progress: the very beginning, clearly behind the seeded 40%.
        sim_path = fs.open_book(seeded)
        # A first sync of a book has no history, so Smart sync shows the Apply/Upload choice.
        sync(fs, "apply")

        for home, progress in (("path-keyed", read_progress_bin(fs.progress_file(sim_path))),
                               ("content-keyed",
                                read_progress_bin(fs.book_state_dir(LIBRARY / seeded["file"]) / "progress.bin"))):
            assert progress is not None, f"{home} progress.bin missing after sync"
            assert progress["spine"] > 0, f"{home} progress did not advance: {progress}"


def scenario_sync_progress_push() -> None:
    """A further local position reaches the server under the simulator's device id."""
    manifest, _ = load_seed()
    seeded = manifest["progress"][1]

    with sd_card(manifest) as fs:
        # Local progress in the last chapter (the fixture has 5 spine items;
        # an out-of-range spine would be clamped back to 0): clearly ahead of
        # the seeded 40%.
        fs.open_book(seeded, progress=(4, 0, 1))
        sync(fs, "upload")

    remote = device(manifest, manifest["peer_device_id"]).get_progress(seeded["hash"])
    assert float(remote.get("percentage", 0)) > 0.5, f"server progress not advanced: {remote}"
    assert remote.get("device_id") == SIM_DEVICE_ID, f"unexpected device id: {remote}"


def scenario_sleep_sync_push() -> None:
    """With Sync on Sleep on, putting the device to sleep uploads the local position
    from behind the sleep screen: sleep hands over to the sleep sync's network boot,
    which joins the saved network on its own and follows the smart rules. Its sync
    marker is the 12-byte one earlier releases wrote, so those are proven to still read."""
    manifest, _ = load_seed()
    seeded = manifest["progress"][2]
    book_hash = seeded["hash"]
    peer = peer_device(manifest)
    # Put the peer's 40% back, so every run has a position of its own to push.
    peer.put_progress(book_hash, seeded["progress"], seeded["percentage"])

    with sd_card(manifest) as fs:
        fs.enable_sync_on_sleep()
        fs.open_book(seeded, progress=(4, 0, 1))
        # This device last synced after the peer's upload, so only the local side has
        # moved since: the smart rules upload instead of leaving it to a manual sync.
        fs.write_sync_marker(book_hash, int(time.time()) + 3600)

        output = sleep_sync(fs)
        assert "Restarting into the BookOrbit sleep sync" in output, "sleep did not hand over to the sleep sync"
        assert "sync scenario finished" in output, "sleep sync never reached its end marker"

    remote = peer.get_progress(book_hash)
    assert float(remote.get("percentage", 0)) > 0.5, f"server progress not advanced: {remote}"
    assert remote.get("device_id") == SIM_DEVICE_ID, f"unexpected device id: {remote}"


def scenario_sleep_sync_after_web_open() -> None:
    """Opening a book in BookOrbit's web reader between two device syncs must not hold
    back the next sleep sync (issue #91). The web reader saves the position it opens
    at, so the server's date moves while its position does not: the device's further
    position is uploaded instead of being left for a manual sync. Two runs on one
    card, so the second sync decides on the history the first one wrote."""
    manifest, _ = load_seed()
    seeded = manifest["progress"][4]
    book_hash = seeded["hash"]
    peer = peer_device(manifest)
    # The peer's 40% is the newest thing on the server again, whatever earlier runs left.
    peer.put_progress(book_hash, seeded["progress"], seeded["percentage"])

    with sd_card(manifest) as fs:
        # 1. A manual sync from chapter 4: a first sync has no history, so the choice
        # screen comes up and the device's position is uploaded.
        sim_path = fs.open_book(seeded, progress=(3, 0, 1))
        sync(fs, "upload", what="first sync")
        marker = fs.read_sync_marker(book_hash)
        assert marker and marker["position"] == (3, 0, 1), \
            f"the upload did not record the synced position: {marker}"

        # 2. The web reader opens the book where the device left it and saves the
        # start of its own page, here the start of that chapter. Guard on the fixture
        # itself: the server must now serve that save, dated after the device's sync.
        served: dict = {}
        for _ in range(10):
            open_in_web_reader(manifest, seeded, "/body/DocFragment[4]/body/p[1]/text().0", 45)
            served = peer.get_progress(book_hash)
            if int(served.get("timestamp", 0)) > marker["timestamp"]:
                break
            time.sleep(1)
        assert served.get("device_id") == "bookorbit-web", f"the web reader's save is not served: {served}"
        assert int(served["timestamp"]) > marker["timestamp"], \
            f"the web reader's save is not dated after the device's sync: {served} vs {marker}"

        # 3. Read on to the last chapter, then put the device to sleep.
        write_progress_bin(fs.progress_file(sim_path), 4, 0, 1)
        fs.enable_sync_on_sleep()
        fs.set_open_book(sim_path)
        output = sleep_sync(fs)
        assert "progress left for the next manual sync" not in output, \
            "a web reader that only opened the book held back the device's progress"

    remote = peer.get_progress(book_hash)
    assert remote.get("device_id") == SIM_DEVICE_ID, f"the device's position was not uploaded: {remote}"
    assert float(remote.get("percentage", 0)) > 0.5, f"server progress not advanced: {remote}"


def scenario_sleep_sync_conflict() -> None:
    """Another reader really read further since this device's last sync, and so did
    this device: nobody is there to arbitrate, so the sleep sync leaves both positions
    and its own history alone. The guard on sleep_sync_after_web_open, which must not
    turn into "the device always wins"."""
    manifest, _ = load_seed()
    seeded = manifest["progress"][5]
    book_hash = seeded["hash"]
    peer = peer_device(manifest)
    now = int(time.time())
    # The peer is in chapter 4 since a moment ago.
    peer.put_progress(book_hash, "/body/DocFragment[4]/body/p[1]/text().0", 0.70, timestamp=now)

    with sd_card(manifest) as fs:
        fs.enable_sync_on_sleep()
        # This device last synced an hour ago in chapter 3, and has reached chapter 5.
        fs.open_book(seeded, progress=(4, 0, 1))
        fs.write_sync_marker(book_hash, now - 3600, position=(2, 0, 1))

        output = sleep_sync(fs)
        assert "Sleep sync: progress left for the next manual sync" in output, \
            "the sleep sync settled a conflict on its own"
        marker = fs.read_sync_marker(book_hash)
        assert marker and marker["position"] == (2, 0, 1), f"an unsettled sync rewrote its history: {marker}"

    remote = peer.get_progress(book_hash)
    assert remote.get("device_id") == manifest["peer_device_id"], \
        f"the other reader's position was overwritten: {remote}"


def scenario_sleep_sync_keeps_progress_reset() -> None:
    """The book's progress was reset in BookOrbit since this device's last sync. The
    server serves that as the start of the book, dated now and under the web reader's
    id, and holds back what a device uploads over it until that device has been to the
    start. The sleep sync must not take it for a web reader that only opened the book:
    the device is further on, and its position stays where it is, unsent; the reset is
    still served afterwards."""
    manifest, _ = load_seed()
    seeded = manifest["progress"][6]
    book_hash = seeded["hash"]
    peer = peer_device(manifest)
    last_sync = int(time.time()) - 3600

    try:
        admin, file_id = web_session(manifest, seeded)
        admin.reset_progress(file_id)
        served = peer.get_progress(book_hash)
        assert served.get("device_id") == "bookorbit-web" and float(served.get("percentage", 1)) == 0, \
            f"the server does not serve the reset: {served}"

        with sd_card(manifest) as fs:
            fs.enable_sync_on_sleep()
            # This device last synced an hour ago in chapter 4, and has reached chapter 5.
            fs.open_book(seeded, progress=(4, 0, 1))
            fs.write_sync_marker(book_hash, last_sync, position=(3, 0, 1))

            output = sleep_sync(fs)
            assert "Sleep sync: progress left for the next manual sync" in output, \
                "the sleep sync did not leave a progress reset to the user"
            # An upload would have rewritten the device's history.
            marker = fs.read_sync_marker(book_hash)
            assert marker == {"timestamp": last_sync, "position": (3, 0, 1)}, \
                f"the sleep sync settled a progress reset on its own: {marker}"

        served = peer.get_progress(book_hash)
        assert served.get("device_id") == "bookorbit-web" and float(served.get("percentage", 1)) == 0, \
            f"the reset is no longer served: {served}"
    finally:
        # Lift the reset for the next run: the server drops it once a device has pushed
        # the start position and then another one.
        peer.put_progress(book_hash, "/body/DocFragment[1]/body", 0.0)
        peer.put_progress(book_hash, seeded["progress"], seeded["percentage"])


def scenario_sleep_sync_cancel() -> None:
    """A Power press during the sleep sync cancels it and wakes the device the way a
    power-button wake would."""
    manifest, _ = load_seed()

    with sd_card(manifest) as fs:
        fs.enable_sync_on_sleep()
        fs.open_book(manifest["progress"][3], progress=None)
        # The 2500 ms press puts the device to sleep, which restarts it into the sleep
        # sync; the press of the second script lands inside that boot, while the sync
        # joins Wi-Fi. The simulator samples the button only when the sync checks for a
        # wake, first as the join ends, and the 20 ms hold completes at a later check, so
        # the press must be down before the sync's requests start: BookOrbit 3.7.0 answers
        # them all within the few hundred milliseconds an earlier press would leave.
        # Woken, the device stays on its home screen, so the run only ends at the timeout.
        output = run_simulator(fs, input_script="2500:POWER:120", choice="none",
                               timeout_s=14, after_reboot_script="100:POWER:600")
        assert "Restarting into the BookOrbit sleep sync" in output, "sleep did not hand over to the sleep sync"
        assert "Sleep sync cancelled: the device was woken" in output, "the press did not cancel the sleep sync"
        cancelled_at = output.index("Sleep sync cancelled")
        assert "Wake route: PowerButton" in output[cancelled_at:], "the cancel did not boot as a power-button wake"
        assert "Sleep sync over" not in output, "a sleep sync ran to its end despite the press"
