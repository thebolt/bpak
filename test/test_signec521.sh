#!/bin/sh
BPAK=../src/bpak
V=-vvvv
echo Sign test ec521
pwd
set -e

$BPAK --help

IMG_A=sign_test_sign_ec521.bpak
PKG_UUID=0888b0fa-9c48-4524-9845-06a641b61edd
PKG_UNIQUE_ID_A=$(uuidgen)
set -e

# Create A package
echo Creating package A
$BPAK create $IMG_A -Y --hash-kind sha512 --signature-kind secp521r1 $V

$BPAK add $IMG_A --meta bpak-package --from-string $PKG_UUID --encoder uuid $V
$BPAK add $IMG_A --meta bpak-package-uid --from-string $PKG_UNIQUE_ID_A \
                 --encoder uuid $V

$BPAK set $IMG_A --key-id pb-development \
                 --keystore-id pb-internal $V

$BPAK sign $IMG_A --key $srcdir/secp521r1-key-pair.pem $V

$BPAK show $IMG_A $V
$BPAK verify $IMG_A --key $srcdir/secp521r1-pub-key.der $V
