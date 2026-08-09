set -e
command -v xz >/dev/null || { apt-get install -y xz-utils >/dev/null 2>&1; }
mkdir -p /br/mmu-trim
cd /br/mmu-trim
if [ ! -d linux-6.1.44 ]; then
  echo "extracting pristine kernel source..."
  tar xf /br/buildroot/dl/linux/linux-6.1.44.tar.xz
fi
# Base config: exactly what the working guest was built with. Read-only use of
# guest-runtime's tree; nothing is written there.
cp /br/mmu/build/linux-6.1.44/.config /br/mmu-trim/config.base
echo "base symbols: $(grep -c '^CONFIG_' /br/mmu-trim/config.base)"
ls -d /br/mmu-trim/linux-6.1.44
