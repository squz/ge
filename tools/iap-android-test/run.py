#!/usr/bin/env python3
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
"""Host JVM oracle for production IapBridge + AndroidStore, with mocked Play.
Requires a JDK (17+), Android SDK/NDK, C++20 compiler, and network on first run.
Downloaded test-only dependencies are pinned and cached under build/.
"""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'build/iap-android-test'
DEPS = BUILD / 'deps'
DEPS.mkdir(parents=True, exist_ok=True)
MAVEN = 'https://repo.maven.apache.org/maven2/'
ARTIFACTS = [
    (MAVEN + 'org/mockito/mockito-core/5.18.0/mockito-core-5.18.0.jar', 'a3d4e40f7fe66016fe42c00de4e343777d74132afc85018e0f03ac6334a60f29'),
    (MAVEN + 'net/bytebuddy/byte-buddy/1.17.5/byte-buddy-1.17.5.jar', '71568c9f8396677219f650268fbf6493ded484edcdbdf2dae6129ca5be81e8db'),
    (MAVEN + 'net/bytebuddy/byte-buddy-agent/1.17.5/byte-buddy-agent-1.17.5.jar', 'c5b9334ad82e632f6af60df22bbbdbbb62cee04877f4f43c38ba04aed9bd9901'),
    (MAVEN + 'org/objenesis/objenesis/3.3/objenesis-3.3.jar', '02dfd0b0439a5591e35b708ed2f5474eb0948f53abf74637e959b8e4ef69bfeb'),
    ('https://dl.google.com/dl/android/maven2/com/android/billingclient/billing/7.1.1/billing-7.1.1.aar', 'ac1d32fa0077c11ad262dd048dde43261fc3059040f152058d21c94603b96505'),
]


def run(*args):
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True, timeout=120)


def main():
    for url, digest in ARTIFACTS:
        path = DEPS / url.rsplit('/', 1)[1]
        if not path.exists():
            with urllib.request.urlopen(url, timeout=60) as response:
                path.write_bytes(response.read())
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != digest:
            raise RuntimeError(f'Checksum mismatch: {path}')
    with zipfile.ZipFile(DEPS / 'billing-7.1.1.aar') as aar:
        (DEPS / 'billing.jar').write_bytes(aar.read('classes.jar'))
    sdk = Path(os.environ.get('ANDROID_HOME', Path.home() / 'Library/Android/sdk'))
    android_jar = sorted((sdk / 'platforms').glob('*/android.jar'))[-1]
    ndk = Path(os.environ['ANDROID_NDK_HOME']) if 'ANDROID_NDK_HOME' in os.environ else sorted((sdk / 'ndk').iterdir())[-1]
    jni = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'))
    # Copy only JNI; importing the entire NDK include tree shadows host libc.
    jni_include = BUILD / 'jni'
    jni_include.mkdir(exist_ok=True)
    (jni_include / 'jni.h').write_bytes((jni / 'jni.h').read_bytes())
    test = ROOT / 'tools/iap-android-test'
    library = BUILD / ('libiap-test.dylib' if sys.platform == 'darwin' else 'libiap-test.so')
    run(os.environ.get('CXX', 'clang++'), '-std=c++20', '-shared', '-fPIC', '-D__ANDROID__',
        '-I' + str(jni_include), '-I' + str(test / 'stubs'), '-I' + str(ROOT / 'include'),
        test / 'native.cpp', '-o', library)
    classes = BUILD / 'classes'
    classes.mkdir(exist_ok=True)
    cp = os.pathsep.join(map(str, [android_jar, *sorted(DEPS.glob('*.jar'))]))
    run('javac', '-cp', cp, '-d', classes,
        ROOT / 'android-shared/src/main/java/ge/IapBridge.java', test / 'IapBridgeTest.java')
    run('java', '--enable-native-access=ALL-UNNAMED',
        '-javaagent:' + str(DEPS / 'mockito-core-5.18.0.jar'),
        '-Djava.library.path=' + str(BUILD), '-cp', str(classes) + os.pathsep + cp,
        'ge.IapBridgeTest')


if __name__ == '__main__':
    main()
