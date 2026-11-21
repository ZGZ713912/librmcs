"""Export a non-loopback development image and its SHA-256 manifest."""
import argparse
import hashlib
import pathlib
import shutil


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("build", type=pathlib.Path)
    parser.add_argument("destination", type=pathlib.Path)
    args = parser.parse_args()
    cache = (args.build / "CMakeCache.txt").read_text()
    # Reject old loopback builds even though new firmware has no loopback option.
    if "EVK_INTERNAL_LOOPBACK:BOOL=ON" in cache:
        raise SystemExit("Refusing to export a legacy loopback build")
    args.destination.mkdir(parents=True, exist_ok=True)
    lines = []
    for extension in ("elf", "bin"):
        source = args.build / "output" / f"demo.{extension}"
        target = args.destination / f"evk_ecat-development.{extension}"
        shutil.copyfile(source, target)
        digest = hashlib.sha256(target.read_bytes()).hexdigest()
        lines.append(f"{digest}  {target.name}\n")
        print(target, target.stat().st_size, digest)
    (args.destination / "SHA256SUMS").write_text("".join(lines))
    shutil.copyfile(args.build / "CMakeCache.txt", args.destination / "build-cache.txt")
    (args.destination / "STATUS.txt").write_text(
        "DEVELOPMENT IMAGE, NOT PRODUCTION ACCEPTED\n"
        "Loopback OFF. OP/PDO, session expiry and peripheral internal loopback tested.\n"
        "External wiring, cold boot, physical cable loss and sustained load unverified.\n"
        "Normal image software-reset boot and SAFEOP-to-OP session recovery passed.\n"
        "No IMU, GPIO or DBUS support. One CAN and one UART.\n"
    )


if __name__ == "__main__":
    main()
