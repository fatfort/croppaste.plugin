# Archived: Issue #1 — Remove reMarkable's QML tree

This file preserves the original GitHub issue and comment thread from
`fatfort/croppaste.plugin#1` before the repository was deleted and
recreated to scrub reMarkable's non-public QML source from history.

The original URLs (`https://github.com/fatfort/croppaste.plugin/issues/1`
and the comment permalinks below) no longer resolve after recreation.

---

## Issue

- **Title:** Remove reMarkable's QML tree
- **Author:** @rmitchellscott
- **Opened:** 2026-05-07T12:24:29Z
- **State at archive time:** open
- **URL (dead after recreate):** https://github.com/fatfort/croppaste.plugin/issues/1

> Please remove reMarkable's non-public source code (QML tree) and the script that extracts it from this public repo.
>
> You're going to ruin this for all of us.

---

## Comments

### @Eeems — 2026-05-20T16:44:12Z
Permalink (dead): https://github.com/fatfort/croppaste.plugin/issues/1#issuecomment-4500583275

> @abaj8494

### @abaj8494 — 2026-05-20T23:21:09Z
Permalink (dead): https://github.com/fatfort/croppaste.plugin/issues/1#issuecomment-4503426089

> Thanks for raising this. I've just removed the QML tree (reference/qml/, reference/qml-dump/) and the extract-qml.py extraction script in 853adb6.
>
> I haven't rewritten the history though. I might do so if you think it is necessary.
>
> Thank you for your work on the Remarkable infrastructure; honoured to have you at the **fort**.
>
> @Eeems, thanks for the ping.

### @Eeems — 2026-05-20T23:24:49Z
Permalink (dead): https://github.com/fatfort/croppaste.plugin/issues/1#issuecomment-4503442793

> @abaj8494 You will need to remove and recreate the repository after rewriting history to properly remove the content as github may keep cached copies of the old commits around.

---

## Resolution

1. `reference/qml/`, `reference/qml-dump/`, and `reference/extract-qml.py`
   removed from the working tree in commit `853adb6` (pre-rewrite hash,
   no longer reachable in the recreated repo).
2. `git filter-repo` used to scrub those paths from every commit in
   history.
3. Original GitHub repo deleted and recreated to evict GitHub's cached
   copies of unreachable commits, per @Eeems' guidance.
4. Cleaned history pushed to the recreated repo.

External archives (GHArchive, etc.) of the original public push events
are outside our control and not addressed by this process.
