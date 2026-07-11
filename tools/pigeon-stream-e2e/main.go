// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// pigeon-stream-e2e — 🎯T11 first vertical slice.
//
// Spins a local pigeon relay, auto-mints a PairingRecord pair (skips the
// human ceremony), Register/Connect with a "video" datagram channel, then
// pumps GE2V-shaped frames (H.264 wire header + payload of realistic
// keyframe/P sizes) for a few seconds and reports throughput + loss.
//
// This is the oracle that the WebSocket streamrelay path must be replaced by:
// video on datagrams, control/input on a reliable stream, E2E encrypted.
//
//	cd tools/pigeon-stream-e2e && go run .
//	# or: make -C sample/tiltbuggy pigeon-stream-e2e
package main

import (
	"context"
	"crypto/ecdh"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"encoding/binary"
	"fmt"
	"log"
	"math/big"
	"net"
	"os"
	"sync/atomic"
	"time"

	"github.com/marcelocantos/pigeon"
	"github.com/marcelocantos/pigeon/crypto"
)

// Match ge Protocol.h kVideoStreamMagic ("GE2V").
const kVideoStreamMagic uint32 = 0x47453256

var dgChannels = map[string]uint64{
	"video": 1,
}

func main() {
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)
	if err := run(); err != nil {
		log.Fatalf("pigeon-stream-e2e: %v", err)
	}
}

func run() error {
	relayURL, teardown, err := startRelay()
	if err != nil {
		return err
	}
	defer teardown()
	time.Sleep(150 * time.Millisecond)
	log.Printf("relay %s", relayURL)

	bid, cid, brec, crec, err := mintPairing(relayURL)
	if err != nil {
		return err
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	tlsCfg := &tls.Config{InsecureSkipVerify: true}

	pairings := map[string]*crypto.PairingRecord{cid.InstanceID(): brec}
	listener, instanceID, err := pigeon.Register(ctx, &pigeon.RegisterArgs{
		Identity: bid,
		Pairing: func(clientInstanceID string) (*crypto.PairingRecord, error) {
			rec, ok := pairings[clientInstanceID]
			if !ok {
				return nil, fmt.Errorf("unknown client %q", clientInstanceID)
			}
			return rec, nil
		},
		Relay:     relayURL,
		TLS:       tlsCfg,
		Datagrams: dgChannels,
	})
	if err != nil {
		return fmt.Errorf("register: %w", err)
	}
	defer listener.Close()
	log.Printf("backend registered instance=%s", instanceID)

	var (
		recvFrames atomic.Uint64
		recvBytes  atomic.Uint64
		recvMax    atomic.Uint64
	)

	// Player (client).
	clientErr := make(chan error, 1)
	go func() {
		sess, err := pigeon.Connect(ctx, &pigeon.ConnectArgs{
			InstanceID: bid.InstanceID(),
			Record:     crec,
			Identity:   cid,
			Relay:      relayURL,
			TLS:        tlsCfg,
			Datagrams:  dgChannels,
		})
		if err != nil {
			clientErr <- fmt.Errorf("connect: %w", err)
			return
		}
		defer sess.Close()

		// Control stream: receive SessionConfig-like hello then ACK.
		ctrl, err := sess.OpenStream(ctx, "control")
		if err != nil {
			clientErr <- fmt.Errorf("open control: %w", err)
			return
		}
		hello, err := ctrl.Recv(ctx)
		if err != nil {
			clientErr <- fmt.Errorf("recv control: %w", err)
			return
		}
		log.Printf("player control hello: %q", hello)
		if err := ctrl.Send([]byte("ok")); err != nil {
			clientErr <- fmt.Errorf("control ack: %w", err)
			return
		}

		video := sess.Datagram("video")
		deadline := time.Now().Add(5 * time.Second)
		for time.Now().Before(deadline) {
			rctx, rcancel := context.WithTimeout(ctx, 500*time.Millisecond)
			frame, err := video.Recv(rctx)
			rcancel()
			if err != nil {
				continue
			}
			if len(frame) < 8 {
				continue
			}
			magic := binary.LittleEndian.Uint32(frame[:4])
			if magic != kVideoStreamMagic {
				clientErr <- fmt.Errorf("bad magic %#x", magic)
				return
			}
			recvFrames.Add(1)
			recvBytes.Add(uint64(len(frame)))
			for {
				old := recvMax.Load()
				if uint64(len(frame)) <= old || recvMax.CompareAndSwap(old, uint64(len(frame))) {
					break
				}
			}
		}
		clientErr <- nil
	}()

	// Server (backend).
	sess, err := listener.Accept(ctx)
	if err != nil {
		return fmt.Errorf("accept: %w", err)
	}
	defer sess.Close()

	ctrl, err := sess.AcceptStream(ctx, "control")
	if err != nil {
		return fmt.Errorf("accept control: %w", err)
	}
	if err := ctrl.Send([]byte("session_config")); err != nil {
		return fmt.Errorf("send control: %w", err)
	}
	if _, err := ctrl.Recv(ctx); err != nil {
		return fmt.Errorf("recv control ack: %w", err)
	}

	video := sess.Datagram("video")
	const (
		fps       = 60
		duration  = 5 * time.Second
		keyEvery  = 60
		keySize   = 700_000 // realistic large IDR (~0.7 MiB) — matches StreamStats
		pSize     = 8_000
		headerLen = 8 + 1 + 4 // MessageHeader + flags + seq
	)
	ticker := time.NewTicker(time.Second / fps)
	defer ticker.Stop()
	end := time.Now().Add(duration)
	var sent, sentBytes uint64
	seq := uint32(0)
	for time.Now().Before(end) {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
			payload := pSize
			key := seq%keyEvery == 0
			if key {
				payload = keySize
			}
			frame := make([]byte, headerLen+payload)
			binary.LittleEndian.PutUint32(frame[0:4], kVideoStreamMagic)
			binary.LittleEndian.PutUint32(frame[4:8], uint32(1+4+payload))
			if key {
				frame[8] = 1
			}
			binary.LittleEndian.PutUint32(frame[9:13], seq)
			// payload left zeroed — size is what matters for MTU/frag.
			if err := video.Send(frame); err != nil {
				log.Printf("video send: %v (seq=%d size=%d)", err, seq, len(frame))
			} else {
				sent++
				sentBytes += uint64(len(frame))
			}
			seq++
		}
	}

	// Drain client.
	select {
	case err := <-clientErr:
		if err != nil {
			return err
		}
	case <-time.After(3 * time.Second):
		return fmt.Errorf("client timed out")
	}

	rf := recvFrames.Load()
	rb := recvBytes.Load()
	rm := recvMax.Load()
	loss := 0.0
	if sent > 0 {
		loss = 100 * float64(sent-rf) / float64(sent)
	}
	log.Printf("RESULT sent=%d recv=%d loss=%.1f%% sent_bytes=%d recv_bytes=%d max_frame=%d fps≈%.0f",
		sent, rf, loss, sentBytes, rb, rm, float64(rf)/duration.Seconds())

	if rf == 0 {
		return fmt.Errorf("received zero video frames")
	}
	// Datagrams may drop under load; require a usable fraction for the smoke.
	if loss > 50 {
		return fmt.Errorf("frame loss too high: %.1f%%", loss)
	}
	log.Printf("OK pigeon video datagram path works for GE2V-sized frames")
	return nil
}

func startRelay() (string, func(), error) {
	cert, err := selfSignedCert()
	if err != nil {
		return "", nil, err
	}
	tlsCfg := &tls.Config{Certificates: []tls.Certificate{cert}}
	wtSrv, err := pigeon.NewWebTransportServer("127.0.0.1:0", tlsCfg, pigeon.Auth{})
	if err != nil {
		return "", nil, err
	}
	udpAddr, err := net.ResolveUDPAddr("udp", "127.0.0.1:0")
	if err != nil {
		_ = wtSrv.Close()
		return "", nil, err
	}
	udpConn, err := net.ListenUDP("udp", udpAddr)
	if err != nil {
		_ = wtSrv.Close()
		return "", nil, err
	}
	port := udpConn.LocalAddr().(*net.UDPAddr).Port
	qSrv := pigeon.NewQUICServer(fmt.Sprintf("127.0.0.1:%d", port), tlsCfg, pigeon.Auth{}, wtSrv.Hub())
	go qSrv.ServeWithTLS(udpConn, tlsCfg)
	teardown := func() {
		_ = qSrv.Close()
		_ = wtSrv.Close()
	}
	return fmt.Sprintf("https://127.0.0.1:%d", port), teardown, nil
}

func selfSignedCert() (tls.Certificate, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return tls.Certificate{}, err
	}
	serial, _ := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128))
	notBefore := time.Now().Add(-time.Hour)
	tmpl := &x509.Certificate{
		SerialNumber: serial,
		DNSNames:     []string{"localhost"},
		IPAddresses:  []net.IP{net.IPv4(127, 0, 0, 1), net.IPv6loopback},
		NotBefore:    notBefore,
		NotAfter:     notBefore.Add(24 * time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		return tls.Certificate{}, err
	}
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}, nil
}

func mintPairing(relayURL string) (bid, cid crypto.Identity, brec, crec *crypto.PairingRecord, err error) {
	dir, err := os.MkdirTemp("", "ge-pigeon-e2e-")
	if err != nil {
		return
	}
	defer os.RemoveAll(dir)
	bid, err = crypto.NewFileIdentity(dir + "/backend-id.json")
	if err != nil {
		return
	}
	cid, err = crypto.NewFileIdentity(dir + "/client-id.json")
	if err != nil {
		return
	}
	bkp, err := crypto.GenerateKeyPair()
	if err != nil {
		return
	}
	ckp, err := crypto.GenerateKeyPair()
	if err != nil {
		return
	}
	cPubB, err := ecdh.X25519().NewPublicKey(ckp.Public.Bytes())
	if err != nil {
		return
	}
	bPubC, err := ecdh.X25519().NewPublicKey(bkp.Public.Bytes())
	if err != nil {
		return
	}
	brec = crypto.NewPairingRecord(cid.InstanceID(), relayURL, bkp, cPubB)
	crec = crypto.NewPairingRecord(bid.InstanceID(), relayURL, ckp, bPubC)
	return bid, cid, brec, crec, nil
}
