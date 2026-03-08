#!/usr/bin/env python3
"""
Patricia Experience Manager
GUI tool for managing Patricia chess engine experience (.exp) learning features.
"""

import customtkinter as ctk
from tkinter import filedialog, messagebox
from typing import Callable
import chess
import chess.pgn
import subprocess
import threading
import queue
import struct
import json
import os
import re

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

POSITION_INDEX_SUFFIX = ".positions.json"
PGN_BACKUP_FILENAME = "patricia_pgn_backup.json"
EXP_SIGNATURE = b"SugaR Experience version 2"
EXP_ENTRY_SIZE = 24
EXP_ENTRY_FMT = "<QiiiH2s"  # key(u64), move(i32), value(i32), depth(i32), count(u16), pad(2)

# ---------------------------------------------------------------------------
# Binary .exp file reader
# ---------------------------------------------------------------------------

def read_exp_entries(filepath: str) -> list[dict]:
    """Read a Patricia .exp binary file and return raw entry dicts."""
    entries = []
    with open(filepath, "rb") as f:
        header = f.read(len(EXP_SIGNATURE))
        if header != EXP_SIGNATURE:
            raise ValueError(
                f"Invalid experience file signature (got {header[:30]!r})"
            )
        while True:
            chunk = f.read(EXP_ENTRY_SIZE)
            if len(chunk) < EXP_ENTRY_SIZE:
                break
            key, move, value, depth, count, _pad = struct.unpack(
                EXP_ENTRY_FMT, chunk
            )
            entries.append(
                {
                    "key": key,
                    "move": move,
                    "value": value,
                    "depth": depth,
                    "count": count,
                }
            )
    return entries


def load_position_index(exp_path: str) -> dict[int, str]:
    """Load the key->FEN position index for an exp file. Returns {int_key: fen}."""
    idx_path = exp_path + POSITION_INDEX_SUFFIX
    if not os.path.isfile(idx_path):
        return {}
    with open(idx_path, "r", encoding="utf-8") as f:
        raw: dict[str, str] = json.load(f)
    return {int(k): v for k, v in raw.items()}


def save_position_index(exp_path: str, index: dict[int, str]):
    """Persist the key->FEN position index alongside the exp file."""
    idx_path = exp_path + POSITION_INDEX_SUFFIX
    raw = {str(k): v for k, v in index.items()}
    with open(idx_path, "w", encoding="utf-8") as f:
        json.dump(raw, f, separators=(",", ":"))


def merge_position_indexes(target_exp_path: str, source_exp_paths: list[str]) -> int:
    """Merge position indexes from target and all sources into target. Returns number of keys added."""
    combined = load_position_index(target_exp_path)
    initial = len(combined)
    for path in source_exp_paths:
        idx = load_position_index(path)
        for k, fen in idx.items():
            if k not in combined:
                combined[k] = fen
    save_position_index(target_exp_path, combined)
    return len(combined) - initial


# ---------------------------------------------------------------------------
# Engine subprocess wrapper
# ---------------------------------------------------------------------------

class EngineProcess:
    """Manages the Patricia engine subprocess with UCI protocol."""

    def __init__(self, log_callback):
        self.process = None
        self._log = log_callback
        self._bestmove_event = threading.Event()
        self._readyok_event = threading.Event()
        self._uciok_event = threading.Event()
        self._last_bestmove = None
        self._reader_thread = None
        self._running = False

        self._collect_lock = threading.Lock()
        self._collecting = False
        self._collected: list[str] = []

    @property
    def is_alive(self) -> bool:
        return self.process is not None and self.process.poll() is None

    # -- lifecycle -----------------------------------------------------------

    def start(self, exe_path: str):
        if self.is_alive:
            self.quit()
        kw = {}
        if os.name == "nt":
            kw["creationflags"] = subprocess.CREATE_NO_WINDOW
        self.process = subprocess.Popen(
            [exe_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            **kw,
        )
        self._running = True
        self._reader_thread = threading.Thread(target=self._read_loop, daemon=True)
        self._reader_thread.start()

    def quit(self):
        self._running = False
        if self.is_alive:
            try:
                self._send_raw("quit")
                self.process.wait(timeout=5)
            except Exception:
                try:
                    self.process.kill()
                except Exception:
                    pass
        self.process = None

    # -- I/O -----------------------------------------------------------------

    def _send_raw(self, cmd: str):
        try:
            self.process.stdin.write(cmd + "\n")
            self.process.stdin.flush()
        except (BrokenPipeError, OSError):
            pass

    def send(self, cmd: str):
        if not self.is_alive:
            return
        self._log(f">>> {cmd}")
        self._send_raw(cmd)

    def _read_loop(self):
        try:
            while self._running:
                line = self.process.stdout.readline()
                if not line:
                    break
                line = line.rstrip("\r\n")
                if not line:
                    continue
                self._log(line)
                with self._collect_lock:
                    if self._collecting:
                        self._collected.append(line)
                if line == "uciok":
                    self._uciok_event.set()
                elif line == "readyok":
                    self._readyok_event.set()
                elif line.startswith("bestmove"):
                    self._last_bestmove = line
                    self._bestmove_event.set()
        except Exception:
            pass

    # -- protocol helpers ----------------------------------------------------

    def init_uci(
        self,
        exp_file: str | None = None,
        options: dict[str, str] | None = None,
    ):
        self._uciok_event.clear()
        self.send("uci")
        if not self._uciok_event.wait(timeout=15):
            self._log("[Warning] Timed out waiting for uciok")

        if options:
            for name, val in options.items():
                self.send(f"setoption name {name} value {val}")

        self.send("setoption name Experience Enabled value true")
        if exp_file:
            self.send(f"setoption name Experience File value {exp_file}")

        self._readyok_event.clear()
        self.send("isready")
        if not self._readyok_event.wait(timeout=30):
            self._log("[Warning] Timed out waiting for readyok")

    def wait_ready(self, timeout=60) -> bool:
        self._readyok_event.clear()
        self.send("isready")
        return self._readyok_event.wait(timeout=timeout)

    def go_depth(self, depth: int):
        self._bestmove_event.clear()
        self._last_bestmove = None
        self.send(f"go depth {depth}")

    def go_movetime(self, ms: int):
        self._bestmove_event.clear()
        self._last_bestmove = None
        self.send(f"go movetime {ms}")

    def wait_bestmove(self, timeout=None) -> str | None:
        self._bestmove_event.wait(timeout=timeout)
        return self._last_bestmove

    def new_game(self):
        self.send("ucinewgame")
        self.wait_ready()

    def set_position_fen(self, fen: str):
        self.send(f"position fen {fen}")

    def stop_search(self):
        if self.is_alive:
            self.send("stop")

    # -- collection mode -----------------------------------------------------

    def start_collecting(self):
        with self._collect_lock:
            self._collected.clear()
            self._collecting = True

    def stop_collecting(self) -> list[str]:
        with self._collect_lock:
            self._collecting = False
            return list(self._collected)

    def get_position_key(self, fen: str, timeout=10) -> int | None:
        """Set a position and retrieve its Zobrist key via the expex command."""
        self.start_collecting()
        self.set_position_fen(fen)
        self.send("expex")
        self._readyok_event.clear()
        self.send("isready")
        self._readyok_event.wait(timeout=timeout)
        lines = self.stop_collecting()

        for ln in lines:
            m = re.search(r"Position key:\s*(\d+)", ln)
            if m:
                return int(m.group(1))
        return None

    def get_current_position_key(self, timeout=10) -> int | None:
        """Retrieve the Zobrist key of the already-set position."""
        self.start_collecting()
        self.send("expex")
        self._readyok_event.clear()
        self.send("isready")
        self._readyok_event.wait(timeout=timeout)
        lines = self.stop_collecting()

        for ln in lines:
            m = re.search(r"Position key:\s*(\d+)", ln)
            if m:
                return int(m.group(1))
        return None

    def get_expex(self, fen: str, timeout=10) -> list[dict]:
        """Query experience for a position. Returns list of entry dicts."""
        self.start_collecting()
        self.set_position_fen(fen)
        self.send("expex")
        self._readyok_event.clear()
        self.send("isready")
        self._readyok_event.wait(timeout=timeout)
        lines = self.stop_collecting()

        pat = re.compile(
            r"\s*\d+:\s+(\S+)\s*,\s*depth:\s*(\d+)\s*,\s*eval:\s*(.*?)\s*,"
            r"\s*count:\s*(\d+)\s*,\s*quality:\s*(-?\d+)"
        )
        entries = []
        for ln in lines:
            m = pat.search(ln)
            if m:
                entries.append(
                    {
                        "move": m.group(1),
                        "depth": int(m.group(2)),
                        "eval": m.group(3).strip(),
                        "count": int(m.group(4)),
                        "quality": int(m.group(5)),
                    }
                )
        return entries

    def get_key_and_has_experience(self, fen: str, timeout=10) -> tuple[int | None, bool]:
        """Set position, send expex; return (key, True) only if engine has experience for this position."""
        self.start_collecting()
        self.set_position_fen(fen)
        self.send("expex")
        self._readyok_event.clear()
        self.send("isready")
        self._readyok_event.wait(timeout=timeout)
        lines = self.stop_collecting()

        key = None
        has_experience = False
        entry_pat = re.compile(r"depth:\s*\d+")
        for ln in lines:
            m = re.search(r"Position key:\s*(\d+)", ln)
            if m:
                key = int(m.group(1))
            if entry_pat.search(ln):
                has_experience = True
        return (key, has_experience)


# ---------------------------------------------------------------------------
# PGN helpers
# ---------------------------------------------------------------------------

def _game_header(game: chess.pgn.Game) -> str:
    """Build a short label from PGN headers, e.g. 'Carlsen vs Caruana'."""
    w = game.headers.get("White", "?")
    b = game.headers.get("Black", "?")
    return f"{w} vs {b}"


GameData = tuple[str, str | None, list[tuple[str, str]]]


def games_from_pgn(pgn_path: str) -> list[GameData]:
    """Return [(label, start_fen_or_None, [(uci_move, fen_after), ...]), ...]

    *start_fen_or_None* is ``None`` when the game starts from the standard
    position (so the GUI can use ``position startpos moves …``), or a FEN
    string for non-standard starts (``position fen <fen> moves …``).
    """
    games: list[GameData] = []
    with open(pgn_path, encoding="utf-8", errors="replace") as f:
        while True:
            game = chess.pgn.read_game(f)
            if game is None:
                break
            label = _game_header(game)
            board = game.board()
            start_fen: str | None = None
            if board.fen() != chess.STARTING_FEN:
                start_fen = board.fen()
            moves: list[tuple[str, str]] = []
            for move in game.mainline_moves():
                uci_str = move.uci()
                board.push(move)
                moves.append((uci_str, board.fen()))
            if moves:
                games.append((label, start_fen, moves))
    return games


def count_pgn_games_and_positions(
    pgn_path: str,
    progress_callback: Callable[[int, int], None] | None = None,
    cancel_check: Callable[[], bool] | None = None,
) -> tuple[int, int]:
    """Stream PGN and count games and positions. Low memory; suitable for huge files.
    Optional progress_callback(games_so_far, positions_so_far) and cancel_check()."""
    total_games = 0
    total_positions = 0
    with open(pgn_path, encoding="utf-8", errors="replace") as f:
        while True:
            if cancel_check and cancel_check():
                break
            game = chess.pgn.read_game(f)
            if game is None:
                break
            n = sum(1 for _ in game.mainline_moves())
            if n > 0:
                total_games += 1
                total_positions += n
            if progress_callback and total_games % 1000 == 0 and total_games > 0:
                progress_callback(total_games, total_positions)
    return (total_games, total_positions)


def games_from_pgn_streaming(pgn_path: str):
    """Yield (label, start_fen_or_None, [(uci_move, fen_after), ...]) one game at a time.
    Low memory; use for large PGNs."""
    with open(pgn_path, encoding="utf-8", errors="replace") as f:
        while True:
            game = chess.pgn.read_game(f)
            if game is None:
                break
            label = _game_header(game)
            board = game.board()
            start_fen: str | None = None
            if board.fen() != chess.STARTING_FEN:
                start_fen = board.fen()
            moves: list[tuple[str, str]] = []
            for move in game.mainline_moves():
                uci_str = move.uci()
                board.push(move)
                moves.append((uci_str, board.fen()))
            if moves:
                yield (label, start_fen, moves)


# ---------------------------------------------------------------------------
# Main GUI
# ---------------------------------------------------------------------------

class PatriciaGUI(ctk.CTk):

    def __init__(self):
        super().__init__()

        self.title("Patricia Experience Manager")
        self.geometry("960x780")
        self.minsize(860, 700)

        ctk.set_appearance_mode("dark")
        ctk.set_default_color_theme("blue")

        self.engine_path_var = ctk.StringVar()
        self.exp_path_var = ctk.StringVar()

        self.opt_hash_var = ctk.StringVar(value="128")
        self.opt_threads_var = ctk.StringVar(value="1")
        self.opt_multipv_var = ctk.StringVar(value="1")
        self.opt_syzygy_var = ctk.StringVar()

        self._log_queue: queue.Queue[str] = queue.Queue()
        self.engine = EngineProcess(self._enqueue_log)
        self._cancel = threading.Event()
        self._worker: threading.Thread | None = None
        self._current_exp: str | None = None
        self._sent_uci_opts: dict[str, str] = {}
        self._all_action_btns: list[ctk.CTkButton] = []
        self._pgn_backup_state: dict | None = None

        self._build_ui()
        self._poll_log()
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # -----------------------------------------------------------------------
    # UI construction
    # -----------------------------------------------------------------------

    def _build_ui(self):
        sf = ctk.CTkFrame(self, corner_radius=8)
        sf.pack(fill="x", padx=10, pady=(10, 4))

        ctk.CTkLabel(sf, text="Universal Settings", font=ctk.CTkFont(size=14, weight="bold")).pack(
            anchor="w", padx=10, pady=(8, 2)
        )

        r1 = ctk.CTkFrame(sf, fg_color="transparent")
        r1.pack(fill="x", padx=10, pady=2)
        ctk.CTkButton(r1, text="Select Engine Executable", width=200, command=self._pick_engine).pack(side="left")
        ctk.CTkEntry(r1, textvariable=self.engine_path_var, state="readonly").pack(
            side="left", fill="x", expand=True, padx=(8, 0)
        )

        r2 = ctk.CTkFrame(sf, fg_color="transparent")
        r2.pack(fill="x", padx=10, pady=2)
        ctk.CTkButton(r2, text="Select Target Exp File (.exp)", width=200, command=self._pick_exp).pack(side="left")
        ctk.CTkEntry(r2, textvariable=self.exp_path_var, state="readonly").pack(
            side="left", fill="x", expand=True, padx=(8, 0)
        )

        ctk.CTkLabel(sf, text="Engine Options", font=ctk.CTkFont(size=12, weight="bold")).pack(
            anchor="w", padx=10, pady=(6, 2)
        )
        orow = ctk.CTkFrame(sf, fg_color="transparent")
        orow.pack(fill="x", padx=10, pady=(0, 4))
        ctk.CTkLabel(orow, text="Hash (MB):").pack(side="left")
        ctk.CTkEntry(orow, textvariable=self.opt_hash_var, width=70).pack(side="left", padx=(4, 14))
        ctk.CTkLabel(orow, text="Threads:").pack(side="left")
        ctk.CTkEntry(orow, textvariable=self.opt_threads_var, width=50).pack(side="left", padx=(4, 14))
        ctk.CTkLabel(orow, text="MultiPV:").pack(side="left")
        ctk.CTkEntry(orow, textvariable=self.opt_multipv_var, width=50).pack(side="left", padx=(4, 14))

        srow = ctk.CTkFrame(sf, fg_color="transparent")
        srow.pack(fill="x", padx=10, pady=(0, 8))
        ctk.CTkButton(srow, text="SyzygyPath", width=100, command=self._pick_syzygy).pack(side="left")
        ctk.CTkEntry(srow, textvariable=self.opt_syzygy_var, state="readonly").pack(
            side="left", fill="x", expand=True, padx=(8, 0)
        )

        self.tabs = ctk.CTkTabview(self, corner_radius=8)
        self.tabs.pack(fill="both", expand=True, padx=10, pady=4)

        self._build_pgn_tab()
        self._build_depth_tab()
        self._build_ops_tab()

        lf = ctk.CTkFrame(self, corner_radius=8)
        lf.pack(fill="both", padx=10, pady=(4, 10), expand=False)

        lh = ctk.CTkFrame(lf, fg_color="transparent")
        lh.pack(fill="x", padx=10, pady=(6, 0))
        ctk.CTkLabel(lh, text="Engine Log", font=ctk.CTkFont(size=13, weight="bold")).pack(side="left")
        ctk.CTkButton(lh, text="Clear Log", width=80, command=self._clear_log).pack(side="right")

        self.log_box = ctk.CTkTextbox(lf, height=180, font=ctk.CTkFont(family="Consolas", size=11))
        self.log_box.pack(fill="both", padx=10, pady=(4, 8), expand=True)

    # -- Tab 1: PGN Analysis ------------------------------------------------

    def _build_pgn_tab(self):
        tab = self.tabs.add("PGN Analysis")

        self.pgn_path_var = ctk.StringVar()
        self.pgn_order_var = ctk.StringVar(value="First to Last")
        self.pgn_limit_type_var = ctk.StringVar(value="Fixed Depth")
        self.pgn_limit_val_var = ctk.StringVar(value="20")

        row = ctk.CTkFrame(tab, fg_color="transparent")
        row.pack(fill="x", padx=10, pady=(10, 4))
        ctk.CTkButton(row, text="Select PGN File", width=160, command=self._pick_pgn).pack(side="left")
        ctk.CTkEntry(row, textvariable=self.pgn_path_var, state="readonly").pack(
            side="left", fill="x", expand=True, padx=(8, 0)
        )

        opts = ctk.CTkFrame(tab, fg_color="transparent")
        opts.pack(fill="x", padx=10, pady=4)
        ctk.CTkLabel(opts, text="Move Order:").pack(side="left")
        ctk.CTkOptionMenu(opts, variable=self.pgn_order_var, values=["First to Last", "Last to First"], width=140).pack(
            side="left", padx=(4, 16)
        )
        ctk.CTkLabel(opts, text="Limit:").pack(side="left")
        ctk.CTkOptionMenu(
            opts, variable=self.pgn_limit_type_var, values=["Fixed Depth", "Fixed Time (sec)"], width=150
        ).pack(side="left", padx=4)
        ctk.CTkEntry(opts, textvariable=self.pgn_limit_val_var, width=80).pack(side="left", padx=4)

        start_row = ctk.CTkFrame(tab, fg_color="transparent")
        start_row.pack(fill="x", padx=10, pady=4)
        ctk.CTkLabel(start_row, text="Start from game #:").pack(side="left")
        self.pgn_start_from_var = ctk.StringVar(value="1")
        ctk.CTkEntry(start_row, textvariable=self.pgn_start_from_var, width=80).pack(side="left", padx=(4, 16))
        ctk.CTkButton(
            start_row, text="Load last progress", width=140, command=self._load_last_pgn_progress,
        ).pack(side="left")

        brow = ctk.CTkFrame(tab, fg_color="transparent")
        brow.pack(fill="x", padx=10, pady=6)
        self.pgn_start_btn = ctk.CTkButton(
            brow, text="Start Analysis", width=150, fg_color="#2fa572", hover_color="#1d7a53",
            command=self._start_pgn_analysis,
        )
        self.pgn_start_btn.pack(side="left", padx=(0, 6))
        self._all_action_btns.append(self.pgn_start_btn)

        self.pgn_stop_btn = ctk.CTkButton(
            brow, text="Stop", width=80, fg_color="#c0392b", hover_color="#96281b",
            command=self._request_cancel, state="disabled",
        )
        self.pgn_stop_btn.pack(side="left")

        self.pgn_progress = ctk.CTkProgressBar(tab)
        self.pgn_progress.pack(fill="x", padx=10, pady=(2, 2))
        self.pgn_progress.set(0)

        self.pgn_status = ctk.CTkLabel(tab, text="Ready", anchor="w")
        self.pgn_status.pack(fill="x", padx=10)

    # -- Tab 2: Experience Depth Upgrader -----------------------------------

    def _build_depth_tab(self):
        tab = self.tabs.add("Depth Upgrader")

        self.du_threshold_var = ctk.StringVar(value="20")
        self.du_target_var = ctk.StringVar(value="30")

        ctk.CTkLabel(
            tab,
            text=(
                "Finds positions in the .exp file recorded below the Depth Threshold\n"
                "and re-analyzes them to the Target Depth.  A position index (built\n"
                "automatically during PGN Analysis) maps experience keys to FENs."
            ),
            justify="left",
            font=ctk.CTkFont(size=11),
        ).pack(padx=10, pady=(10, 6), anchor="w")

        # Optional: build position index from PGN (only positions that have experience)
        self.du_pgn_path_var = ctk.StringVar()
        du_opt_frame = ctk.CTkFrame(tab, fg_color=("gray85", "gray25"))
        du_opt_frame.pack(fill="x", padx=10, pady=(4, 8))
        ctk.CTkLabel(
            du_opt_frame,
            text="Optional: Build position index from PGN (only positions with experience in the .exp file)",
            font=ctk.CTkFont(size=11),
        ).pack(anchor="w", padx=10, pady=(8, 4))
        du_pgn_row = ctk.CTkFrame(du_opt_frame, fg_color="transparent")
        du_pgn_row.pack(fill="x", padx=10, pady=(0, 8))
        ctk.CTkLabel(du_pgn_row, text="PGN file:").pack(side="left")
        ctk.CTkEntry(du_pgn_row, textvariable=self.du_pgn_path_var, state="readonly", width=400).pack(
            side="left", padx=(6, 6)
        )
        ctk.CTkButton(du_pgn_row, text="Browse…", width=80, command=self._pick_du_pgn).pack(side="left")
        self.du_getkeys_btn = ctk.CTkButton(
            du_opt_frame,
            text="Get keys from PGN",
            width=160,
            fg_color="#3498db",
            hover_color="#2980b9",
            command=self._start_du_getkeys,
        )
        self.du_getkeys_btn.pack(side="left", padx=10, pady=(0, 8))
        self._all_action_btns.append(self.du_getkeys_btn)

        row = ctk.CTkFrame(tab, fg_color="transparent")
        row.pack(fill="x", padx=10, pady=4)
        ctk.CTkLabel(row, text="Depth Threshold (find below):").pack(side="left")
        ctk.CTkEntry(row, textvariable=self.du_threshold_var, width=80).pack(side="left", padx=(6, 24))
        ctk.CTkLabel(row, text="Target Depth:").pack(side="left")
        ctk.CTkEntry(row, textvariable=self.du_target_var, width=80).pack(side="left", padx=6)

        brow = ctk.CTkFrame(tab, fg_color="transparent")
        brow.pack(fill="x", padx=10, pady=8)
        self.du_start_btn = ctk.CTkButton(
            brow, text="Upgrade Positions", width=180, fg_color="#2fa572", hover_color="#1d7a53",
            command=self._start_depth_upgrade,
        )
        self.du_start_btn.pack(side="left", padx=(0, 6))
        self._all_action_btns.append(self.du_start_btn)

        self.du_stop_btn = ctk.CTkButton(
            brow, text="Stop", width=80, fg_color="#c0392b", hover_color="#96281b",
            command=self._request_cancel, state="disabled",
        )
        self.du_stop_btn.pack(side="left")

        self.du_progress = ctk.CTkProgressBar(tab)
        self.du_progress.pack(fill="x", padx=10, pady=(2, 2))
        self.du_progress.set(0)

        self.du_status = ctk.CTkLabel(tab, text="Ready", anchor="w")
        self.du_status.pack(fill="x", padx=10)

    # -- Tab 3: Operations & Maintenance ------------------------------------

    def _build_ops_tab(self):
        tab = self.tabs.add("Operations & Maintenance")

        ctk.CTkLabel(tab, text="Experience File Operations", font=ctk.CTkFont(size=14, weight="bold")).pack(
            padx=10, pady=(14, 10), anchor="w"
        )

        b1 = ctk.CTkButton(
            tab, text="Merge Experience Files  (Keep Quality)", width=340,
            command=self._do_merge,
        )
        b1.pack(padx=10, pady=5, anchor="w")
        self._all_action_btns.append(b1)

        b2 = ctk.CTkButton(
            tab, text="Merge Experience Files  (Discard Quality)", width=340,
            command=self._do_merge_nq,
        )
        b2.pack(padx=10, pady=5, anchor="w")
        self._all_action_btns.append(b2)

        b3 = ctk.CTkButton(
            tab, text="Defrag Experience File", width=340,
            command=self._do_defrag,
        )
        b3.pack(padx=10, pady=5, anchor="w")
        self._all_action_btns.append(b3)

        self.ops_status = ctk.CTkLabel(tab, text="Ready", anchor="w")
        self.ops_status.pack(fill="x", padx=10, pady=(10, 0))

    # -----------------------------------------------------------------------
    # File pickers
    # -----------------------------------------------------------------------

    def _pick_engine(self):
        p = filedialog.askopenfilename(
            title="Select Patricia Engine Executable",
            filetypes=[("Executable", "*.exe"), ("All files", "*.*")],
        )
        if p:
            self.engine_path_var.set(p)

    def _pick_exp(self):
        p = filedialog.askopenfilename(
            title="Select Target Experience File",
            filetypes=[("Experience file", "*.exp"), ("All files", "*.*")],
        )
        if p:
            if " " in p:
                messagebox.showwarning(
                    "Path Warning",
                    "The selected path contains spaces. The engine may not handle this correctly.\n"
                    "Consider moving the file to a path without spaces.",
                )
            self.exp_path_var.set(p)

    def _pick_pgn(self):
        p = filedialog.askopenfilename(
            title="Select PGN File",
            filetypes=[("PGN files", "*.pgn"), ("All files", "*.*")],
        )
        if p:
            self.pgn_path_var.set(p)

    def _pick_du_pgn(self):
        p = filedialog.askopenfilename(
            title="Select PGN File (for position index)",
            filetypes=[("PGN files", "*.pgn"), ("All files", "*.*")],
        )
        if p:
            self.du_pgn_path_var.set(p)

    def _pick_syzygy(self):
        d = filedialog.askdirectory(title="Select Syzygy Tablebases Folder")
        if d:
            self.opt_syzygy_var.set(d)

    # -----------------------------------------------------------------------
    # Logging
    # -----------------------------------------------------------------------

    def _enqueue_log(self, msg: str):
        self._log_queue.put(msg)

    def _poll_log(self):
        try:
            for _ in range(200):
                msg = self._log_queue.get_nowait()
                self.log_box.insert("end", msg + "\n")
                self.log_box.see("end")
        except queue.Empty:
            pass
        self.after(80, self._poll_log)

    def _clear_log(self):
        self.log_box.delete("1.0", "end")

    # -----------------------------------------------------------------------
    # Validation & engine lifecycle
    # -----------------------------------------------------------------------

    def _validate_global(self) -> bool:
        if not self.engine_path_var.get():
            messagebox.showerror("Missing Input", "Please select the Patricia engine executable first.")
            return False
        if not os.path.isfile(self.engine_path_var.get()):
            messagebox.showerror("File Not Found", "Engine executable does not exist at the specified path.")
            return False
        if not self.exp_path_var.get():
            messagebox.showerror("Missing Input", "Please select a target experience (.exp) file first.")
            return False
        return True

    def _gather_uci_options(self) -> dict[str, str]:
        """Collect current UCI option values from the GUI."""
        opts: dict[str, str] = {}
        h = self.opt_hash_var.get().strip()
        if h:
            opts["Hash"] = h
        t = self.opt_threads_var.get().strip()
        if t:
            opts["Threads"] = t
        m = self.opt_multipv_var.get().strip()
        if m:
            opts["MultiPV"] = m
        s = self.opt_syzygy_var.get().strip()
        if s:
            opts["SyzygyPath"] = s
        return opts

    def _ensure_engine(self):
        """Start engine if needed and configure experience file + UCI options."""
        exp = self.exp_path_var.get()
        opts = self._gather_uci_options()

        if not self.engine.is_alive:
            self.engine.start(self.engine_path_var.get())
            self.engine.init_uci(exp, options=opts)
            self._current_exp = exp
            self._sent_uci_opts = dict(opts)
            return

        changed = False
        if self._current_exp != exp:
            self.engine.send(f"setoption name Experience File value {exp}")
            self._current_exp = exp
            changed = True

        for name, val in opts.items():
            if self._sent_uci_opts.get(name) != val:
                self.engine.send(f"setoption name {name} value {val}")
                self._sent_uci_opts[name] = val
                changed = True

        if changed:
            self.engine.wait_ready()

    # -----------------------------------------------------------------------
    # Worker thread helpers
    # -----------------------------------------------------------------------

    def _is_busy(self) -> bool:
        return self._worker is not None and self._worker.is_alive()

    def _request_cancel(self):
        self._cancel.set()
        self.engine.stop_search()

    def _set_busy(self, stop_btn: ctk.CTkButton):
        self._cancel.clear()
        for b in self._all_action_btns:
            b.configure(state="disabled")
        stop_btn.configure(state="normal")

    def _set_idle(self, stop_btn: ctk.CTkButton):
        for b in self._all_action_btns:
            b.configure(state="normal")
        stop_btn.configure(state="disabled")

    def _parse_limit(self, type_var: ctk.StringVar, val_var: ctk.StringVar) -> tuple[str, int] | None:
        """Returns ('depth', N) or ('movetime', ms) or None on error."""
        try:
            v = int(val_var.get())
            if v <= 0:
                raise ValueError
        except ValueError:
            messagebox.showerror("Invalid Input", "Limit value must be a positive integer.")
            return None
        if type_var.get().startswith("Fixed Depth"):
            return ("depth", v)
        return ("movetime", v * 1000)

    def _get_pgn_backup_path(self) -> str:
        return os.path.join(os.path.dirname(os.path.abspath(__file__)), PGN_BACKUP_FILENAME)

    def _save_pgn_backup(self) -> None:
        """Write current PGN backup state to disk (call from main thread or worker)."""
        if not self._pgn_backup_state:
            return
        path = self._get_pgn_backup_path()
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump(self._pgn_backup_state, f, indent=2)
        except OSError as e:
            self._enqueue_log(f"[PGN] Could not save backup: {e}")

    def _load_pgn_backup(self) -> dict | None:
        """Load backup state from disk; returns None if missing or invalid."""
        path = self._get_pgn_backup_path()
        if not os.path.isfile(path):
            return None
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
            if isinstance(data, dict) and "pgn_path" in data and "exp_path" in data:
                return data
        except (OSError, json.JSONDecodeError):
            pass
        return None

    def _load_last_pgn_progress(self) -> None:
        """Load last progress from backup and set PGN path, exp path, and start-from game."""
        data = self._load_pgn_backup()
        if not data:
            messagebox.showinfo("No backup", "No saved PGN progress found.")
            return
        self.pgn_path_var.set(data.get("pgn_path", ""))
        self.exp_path_var.set(data.get("exp_path", ""))
        last = data.get("last_completed_game_1based", 0)
        total = data.get("total_games", 0)
        next_game = min(last + 1, total) if total else 1
        self.pgn_start_from_var.set(str(next_game))
        self._enqueue_log(
            f"[PGN] Loaded backup: through game {last} of {total}; start from game #{next_game}."
        )

    # -----------------------------------------------------------------------
    # Module 1 – PGN Analysis (also builds position index)
    # -----------------------------------------------------------------------

    def _start_pgn_analysis(self):
        if self._is_busy():
            return
        if not self._validate_global():
            return
        pgn = self.pgn_path_var.get()
        if not pgn or not os.path.isfile(pgn):
            messagebox.showerror("Missing Input", "Please select a valid PGN file.")
            return
        limit = self._parse_limit(self.pgn_limit_type_var, self.pgn_limit_val_var)
        if limit is None:
            return
        try:
            start_from = max(1, int(self.pgn_start_from_var.get().strip()))
        except ValueError:
            messagebox.showerror("Invalid Input", "Start from game must be a positive integer.")
            return

        reverse = self.pgn_order_var.get() == "Last to First"

        self._set_busy(self.pgn_stop_btn)
        self._worker = threading.Thread(
            target=self._pgn_worker, args=(pgn, reverse, limit, start_from), daemon=True
        )
        self._worker.start()

    def _pgn_worker(self, pgn_path: str, reverse: bool, limit: tuple[str, int], start_from_game_1based: int):
        try:
            self._update_status(self.pgn_status, "Parsing PGN...")
            self._update_progress(self.pgn_progress, 0)

            all_games = games_from_pgn(pgn_path)
            num_games = len(all_games)
            if num_games == 0:
                self._update_status(self.pgn_status, "No games found in PGN.")
                return

            start_gi = max(0, min(start_from_game_1based - 1, num_games - 1))
            games_to_do = all_games[start_gi:]
            games_count = len(games_to_do)
            total_positions = sum(len(moves) for _, _, moves in games_to_do)
            self._enqueue_log(
                f"[PGN] {num_games} games total; starting from game #{start_gi + 1} — "
                f"{games_count} games, {total_positions} positions."
            )

            self._ensure_engine()

            exp_path = self.exp_path_var.get()
            pos_index = load_position_index(exp_path)
            index_dirty = False
            kind, value = limit
            cancelled = False
            last_completed_game_1based = start_gi  # 1-based index of last fully completed game

            for idx, (label, start_fen, moves) in enumerate(games_to_do):
                gi = start_gi + idx
                if self._cancel.is_set():
                    cancelled = True
                    break

                game_num = gi + 1
                num_moves = len(moves)
                self.engine.new_game()
                self._enqueue_log(
                    f"[PGN] Game {game_num}/{num_games}: {label} ({num_moves} moves)"
                )

                # Build the position prefix once per game
                if start_fen:
                    pos_prefix = f"position fen {start_fen} moves"
                else:
                    pos_prefix = "position startpos moves"

                # Determine analysis order
                if reverse:
                    order = list(range(num_moves - 1, -1, -1))
                else:
                    order = list(range(num_moves))

                for step, mi in enumerate(order):
                    if self._cancel.is_set():
                        cancelled = True
                        break

                    self._update_status(
                        self.pgn_status,
                        f"Game {game_num}/{num_games}  move {mi + 1}/{num_moves}  —  {label}",
                    )
                    self._update_progress(
                        self.pgn_progress,
                        (idx + (step + 1) / num_moves) / games_count,
                    )

                    # Send the full move chain up to move mi
                    uci_chain = " ".join(m for m, _ in moves[: mi + 1])
                    self.engine.send(f"{pos_prefix} {uci_chain}")

                    if kind == "depth":
                        self.engine.go_depth(value)
                    else:
                        self.engine.go_movetime(value)

                    bm = self.engine.wait_bestmove(timeout=600)
                    if bm is None and not self._cancel.is_set():
                        self._enqueue_log(
                            f"[PGN] Warning: no bestmove  game {game_num} move {mi + 1}"
                        )

                    # Position index: key from current engine position, FEN from python-chess
                    _, fen_after = moves[mi]
                    pkey = self.engine.get_current_position_key()
                    if pkey is not None and pkey not in pos_index:
                        pos_index[pkey] = fen_after
                        index_dirty = True

                if cancelled:
                    break

                last_completed_game_1based = game_num
                self._pgn_backup_state = {
                    "pgn_path": pgn_path,
                    "exp_path": exp_path,
                    "last_completed_game_1based": last_completed_game_1based,
                    "total_games": num_games,
                }
                self._save_pgn_backup()
                self._enqueue_log(
                    f"[PGN] Finished game {game_num}/{num_games}: {label}"
                )

            if cancelled:
                self._pgn_backup_state = {
                    "pgn_path": pgn_path,
                    "exp_path": exp_path,
                    "last_completed_game_1based": last_completed_game_1based,
                    "total_games": num_games,
                }
                self._save_pgn_backup()

            if index_dirty:
                save_position_index(exp_path, pos_index)
                self._enqueue_log(
                    f"[PGN] Position index updated ({len(pos_index)} entries)."
                )

            if not cancelled:
                self._update_status(
                    self.pgn_status,
                    f"Done — {games_count} games ({total_positions} positions) analyzed.",
                )
                self._enqueue_log("[PGN] Analysis complete.")
            else:
                self._enqueue_log("[PGN] Cancelled by user.")
                self._update_status(self.pgn_status, "Cancelled.")

        except Exception as exc:
            self._enqueue_log(f"[PGN] Error: {exc}")
            self._update_status(self.pgn_status, f"Error: {exc}")
        finally:
            self.after(0, lambda: self._set_idle(self.pgn_stop_btn))

    # -----------------------------------------------------------------------
    # Module 2 – Experience Depth Upgrader
    # -----------------------------------------------------------------------

    def _start_depth_upgrade(self):
        if self._is_busy():
            return
        if not self._validate_global():
            return
        exp_path = self.exp_path_var.get()
        if not os.path.isfile(exp_path):
            messagebox.showerror("File Not Found", "The target .exp file does not exist.")
            return
        try:
            threshold = int(self.du_threshold_var.get())
            target = int(self.du_target_var.get())
            if threshold <= 0 or target <= 0:
                raise ValueError
            if target <= threshold:
                messagebox.showerror(
                    "Invalid Input",
                    "Target Depth must be greater than the Depth Threshold.",
                )
                return
        except ValueError:
            messagebox.showerror("Invalid Input", "Depth values must be positive integers.")
            return

        self._set_busy(self.du_stop_btn)
        self._worker = threading.Thread(
            target=self._du_worker, args=(exp_path, threshold, target), daemon=True
        )
        self._worker.start()

    def _start_du_getkeys(self):
        if self._is_busy():
            return
        if not self._validate_global():
            return
        pgn_path = self.du_pgn_path_var.get().strip()
        if not pgn_path:
            messagebox.showerror("Missing Input", "Please select a PGN file for the position index.")
            return
        if not os.path.isfile(pgn_path):
            messagebox.showerror("File Not Found", "The selected PGN file does not exist.")
            return
        exp_path = self.exp_path_var.get()
        self._set_busy(self.du_stop_btn)
        self._worker = threading.Thread(
            target=self._du_getkeys_worker, args=(exp_path, pgn_path), daemon=True
        )
        self._worker.start()

    def _du_getkeys_worker(self, exp_path: str, pgn_path: str):
        try:
            self._update_status(self.du_status, "Counting PGN (large file may take a while)...")
            self._update_progress(self.du_progress, 0)

            def on_count_progress(games_so_far: int, positions_so_far: int):
                self._update_status(
                    self.du_status,
                    f"Counting PGN: {games_so_far} games, {positions_so_far} positions…",
                )

            total_games, total_positions = count_pgn_games_and_positions(
                pgn_path,
                progress_callback=on_count_progress,
                cancel_check=self._cancel.is_set,
            )

            if self._cancel.is_set():
                self._enqueue_log("[DU] Get keys cancelled by user.")
                return
            if total_positions == 0:
                self._update_status(self.du_status, "No positions found in PGN.")
                self._enqueue_log("[DU] No positions in PGN.")
                return

            self._enqueue_log(
                f"[DU] Get keys: {total_games} games, {total_positions} positions (streaming)."
            )
            pos_index = load_position_index(exp_path)
            self._ensure_engine()

            done = 0
            added = 0
            game_idx = 0
            for label, start_fen, moves in games_from_pgn_streaming(pgn_path):
                if self._cancel.is_set():
                    self._enqueue_log("[DU] Get keys cancelled by user.")
                    break
                game_idx += 1
                for uci_move, fen_after in moves:
                    if self._cancel.is_set():
                        break
                    key, has_exp = self.engine.get_key_and_has_experience(fen_after, timeout=15)
                    if key is not None and has_exp:
                        if key not in pos_index:
                            added += 1
                        pos_index[key] = fen_after
                    done += 1
                    # Progress bar every 50 positions
                    if done % 50 == 0 or done == total_positions:
                        self._update_progress(self.du_progress, done / total_positions)
                    # Status every 100 positions (and at end) so you see where we are
                    if done % 100 == 0 or done == total_positions:
                        self._update_status(
                            self.du_status,
                            f"Game {game_idx}/{total_games}  pos {done}/{total_positions}  keys +{added}",
                        )

            save_position_index(exp_path, pos_index)
            self._enqueue_log(
                f"[DU] Position index updated: {len(pos_index)} total ({added} new from PGN)."
            )
            if not self._cancel.is_set():
                self._update_status(
                    self.du_status,
                    f"Done — {added} keys added from PGN (index now has {len(pos_index)} entries).",
                )
            else:
                self._update_status(self.du_status, "Cancelled.")
        except Exception as exc:
            self._enqueue_log(f"[DU] Get keys error: {exc}")
            self._update_status(self.du_status, f"Error: {exc}")
        finally:
            self.after(0, lambda: self._set_idle(self.du_stop_btn))

    def _du_worker(self, exp_path: str, threshold: int, target_depth: int):
        try:
            self._update_status(self.du_status, "Reading .exp file...")
            self._update_progress(self.du_progress, 0)

            entries = read_exp_entries(exp_path)
            self._enqueue_log(f"[DU] Loaded {len(entries)} entries from {os.path.basename(exp_path)}")

            # Group entries by position key; find max depth per key
            key_max_depth: dict[int, int] = {}
            for e in entries:
                k = e["key"]
                if k not in key_max_depth or e["depth"] > key_max_depth[k]:
                    key_max_depth[k] = e["depth"]

            below = {k for k, d in key_max_depth.items() if d < threshold}
            self._enqueue_log(
                f"[DU] {len(key_max_depth)} unique positions total, "
                f"{len(below)} below depth {threshold}"
            )

            if not below:
                self._update_status(
                    self.du_status,
                    f"No positions found below depth {threshold}.",
                )
                return

            # Load position index
            self._update_status(self.du_status, "Loading position index...")
            pos_index = load_position_index(exp_path)

            resolvable = {k for k in below if k in pos_index}
            unresolved = len(below) - len(resolvable)

            if not resolvable:
                msg = (
                    f"Found {len(below)} positions below depth {threshold} but none "
                    "have a known FEN.  Run PGN Analysis first to build the position index."
                )
                self._enqueue_log(f"[DU] {msg}")
                self._update_status(self.du_status, msg)
                return

            if unresolved:
                self._enqueue_log(
                    f"[DU] {unresolved} positions could not be resolved to FENs "
                    "(not in the position index)."
                )
                self._enqueue_log(
                    "[DU] Tip: The position index is built by PGN Analysis and by merging "
                    ".exp files (their .positions.json are merged). Run PGN Analysis or merge "
                    "files that have position indexes to make more positions resolvable."
                )

            self._enqueue_log(
                f"[DU] Re-analyzing {len(resolvable)} positions to depth {target_depth}"
            )

            # Start engine and re-analyze
            self._ensure_engine()
            self.engine.new_game()

            ordered = sorted(resolvable)
            ra_total = len(ordered)

            for j, key in enumerate(ordered):
                if self._cancel.is_set():
                    self._enqueue_log("[DU] Cancelled by user.")
                    break

                fen = pos_index[key]
                self._update_status(
                    self.du_status,
                    f"Upgrading {j + 1}/{ra_total}  (depth {key_max_depth[key]} -> {target_depth})",
                )
                self._update_progress(self.du_progress, (j + 1) / ra_total)

                self.engine.set_position_fen(fen)
                self.engine.go_depth(target_depth)

                bm = self.engine.wait_bestmove(timeout=1200)
                if bm is None and not self._cancel.is_set():
                    self._enqueue_log(f"[DU] Warning: no bestmove for position {j + 1}")

            if not self._cancel.is_set():
                summary = f"Done — {ra_total} positions upgraded to depth {target_depth}."
                if unresolved:
                    summary += f"  ({unresolved} skipped, no FEN)"
                self._update_status(self.du_status, summary)
                self._enqueue_log("[DU] Depth upgrade complete.")
            else:
                self._update_status(self.du_status, "Cancelled.")

        except Exception as exc:
            self._enqueue_log(f"[DU] Error: {exc}")
            self._update_status(self.du_status, f"Error: {exc}")
        finally:
            self.after(0, lambda: self._set_idle(self.du_stop_btn))

    # -----------------------------------------------------------------------
    # Module 3 – Operations
    # -----------------------------------------------------------------------

    def _do_merge(self):
        self._merge_common(keep_quality=True)

    def _do_merge_nq(self):
        self._merge_common(keep_quality=False)

    def _merge_common(self, keep_quality: bool):
        if self._is_busy():
            return
        if not self._validate_global():
            return
        sources = filedialog.askopenfilenames(
            title="Select Experience Files to Merge",
            filetypes=[("Experience file", "*.exp"), ("All files", "*.*")],
        )
        if not sources:
            return

        target = self.exp_path_var.get()
        cmd_name = "merge" if keep_quality else "mergenq"
        args = " ".join([target] + list(sources))

        self._cancel.clear()
        for b in self._all_action_btns:
            b.configure(state="disabled")

        def worker():
            try:
                self._update_status(self.ops_status, f"Running {cmd_name}...")
                self._ensure_engine()
                self.engine.send(f"{cmd_name} {args}")
                self.engine.wait_ready(timeout=300)
                # Merge position indexes so Depth Upgrade can resolve keys from merged files
                added = merge_position_indexes(target, list(sources))
                if added > 0:
                    self._enqueue_log(f"[OPS] Position index merged: {added} keys added from source(s).")
                self._update_status(self.ops_status, f"{cmd_name} complete.")
                self._enqueue_log(f"[OPS] {cmd_name} finished.")
            except Exception as exc:
                self._enqueue_log(f"[OPS] Error: {exc}")
                self._update_status(self.ops_status, f"Error: {exc}")
            finally:
                self.after(0, self._restore_ops_idle)

        self._worker = threading.Thread(target=worker, daemon=True)
        self._worker.start()

    def _do_defrag(self):
        if self._is_busy():
            return
        if not self._validate_global():
            return

        target = self.exp_path_var.get()

        self._cancel.clear()
        for b in self._all_action_btns:
            b.configure(state="disabled")

        def worker():
            try:
                self._update_status(self.ops_status, "Defragmenting...")
                self._ensure_engine()
                self.engine.send(f"defrag {target}")
                self.engine.wait_ready(timeout=300)
                self._update_status(self.ops_status, "Defragmentation complete.")
                self._enqueue_log("[OPS] Defrag finished.")
            except Exception as exc:
                self._enqueue_log(f"[OPS] Error: {exc}")
                self._update_status(self.ops_status, f"Error: {exc}")
            finally:
                self.after(0, self._restore_ops_idle)

        self._worker = threading.Thread(target=worker, daemon=True)
        self._worker.start()

    def _restore_ops_idle(self):
        for b in self._all_action_btns:
            b.configure(state="normal")

    # -----------------------------------------------------------------------
    # Thread-safe GUI updates
    # -----------------------------------------------------------------------

    def _update_status(self, label: ctk.CTkLabel, text: str):
        self.after(0, lambda: label.configure(text=text))

    def _update_progress(self, bar: ctk.CTkProgressBar, value: float):
        self.after(0, lambda: bar.set(max(0.0, min(1.0, value))))

    # -----------------------------------------------------------------------
    # Shutdown
    # -----------------------------------------------------------------------

    def _on_close(self):
        self._save_pgn_backup()
        self._cancel.set()
        self.engine.quit()
        self.destroy()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    app = PatriciaGUI()
    app.mainloop()


if __name__ == "__main__":
    main()
