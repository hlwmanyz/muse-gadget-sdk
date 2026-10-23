// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Sends the maker's SDK token to a flashed gadget over Web Serial. The web
// flasher's builds have no token of their own, and a gadget needs one to pair.
// The gadget stores it in NVS and restarts with it (esp32/main/sdk_token_console.h).

// The same check as esp32/cmake/validate_config.cmake: "mgst_", then base64url
// of 32 bytes, whose last character carries only four bits.
export const isSdkToken = (token) => /^mgst_[A-Za-z0-9_-]{42}[AEIMQUYcgkosw048]$/.test(token);

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// Resolves when the gadget has stored the token; rejects with the reason
// otherwise. Opening the port can restart the gadget, so the command is sent
// again until it answers, for up to `timeoutMs`.
export async function sendSdkToken(port, token, { timeoutMs = 25000, retryMs = 3000 } = {}) {
  if (!isSdkToken(token)) throw new Error("That isn't an SDK token. Copy it again from gadgets.muse.ai.");
  try {
    await port.open({ baudRate: 115200 });
  } catch (e) {
    throw new Error(`Couldn't open the port (${e.message}). Close the installer's dialog first.`);
  }
  // Leave EN and BOOT alone on boards whose bridge drives them.
  await port.setSignals({ dataTerminalReady: false, requestToSend: false }).catch(() => {});

  const reader = port.readable.getReader();
  let answer = null;
  const reading = (async () => {
    const decoder = new TextDecoder();
    let text = "";
    for (;;) {
      const { value, done } = await reader.read();
      if (done) return;
      text += decoder.decode(value, { stream: true });
      let nl;
      while ((nl = text.indexOf("\n")) >= 0) {
        const line = text.slice(0, nl).replace(/\r$/, "");
        text = text.slice(nl + 1);
        const m = line.match(/@sdk_token (ok|error \S+)/);
        if (m) answer = m[1];
      }
    }
  })().catch(() => {});

  try {
    const deadline = Date.now() + timeoutMs;
    await sleep(1000);   // a gadget restarted by the port opening boots first
    while (!answer && Date.now() < deadline) {
      const writer = port.writable.getWriter();
      try {
        await writer.write(new TextEncoder().encode(`\n>sdk_token=${token}\n`));
      } finally {
        writer.releaseLock();
      }
      const wait = Date.now() + retryMs;
      while (!answer && Date.now() < wait) await sleep(100);
    }
  } finally {
    await reader.cancel().catch(() => {});
    await reading;
    reader.releaseLock();
    await port.close().catch(() => {});
  }

  if (answer === "ok") return;
  if (answer === "error invalid") throw new Error("The gadget says that isn't an SDK token.");
  if (answer) throw new Error(`The gadget couldn't store the token (${answer}).`);
  throw new Error(
    "The gadget didn't answer. Check it runs firmware from this page and you picked its console port. " +
      "If it still doesn't answer, unplug it, plug it back in and try again.",
  );
}

// The gadget's speaker switch, read and set over the same console port:
// ">" setup commands (esp32/components/muse/muse_input.c, muse_ble.c). The
// gadget's own "@status" reply confirms the state, so the checkbox on the
// page only ever shows what the gadget itself reports — never a guess.

// want: true/false to set the switch, null to only read it. Resolves with
// the gadget's confirmed speaker state. Opening the port can restart the
// gadget, so the commands repeat until it answers, like sendSdkToken.
export async function gadgetSpeaker(port, want, { timeoutMs = 20000, retryMs = 2500 } = {}) {
  try {
    await port.open({ baudRate: 115200 });
  } catch (e) {
    throw new Error(`Couldn't open the port (${e.message}). Close the installer's dialog first.`);
  }
  // Leave EN and BOOT alone on boards whose bridge drives them.
  await port.setSignals({ dataTerminalReady: false, requestToSend: false }).catch(() => {});

  const reader = port.readable.getReader();
  let heard = null;   // last speaker state the gadget itself reported
  const reading = (async () => {
    const decoder = new TextDecoder();
    let text = "";
    for (;;) {
      const { value, done } = await reader.read();
      if (done) return;
      text += decoder.decode(value, { stream: true });
      let nl;
      while ((nl = text.indexOf("\n")) >= 0) {
        const line = text.slice(0, nl).replace(/\r$/, "");
        text = text.slice(nl + 1);
        if (!line.startsWith("@status ")) continue;
        const m = line.match(/"speaker":(true|false)/);
        if (m) heard = m[1] === "true";
      }
    }
  })().catch(() => {});

  const write = async (s) => {
    const writer = port.writable.getWriter();
    try {
      await writer.write(new TextEncoder().encode(s));
    } finally {
      writer.releaseLock();
    }
  };

  try {
    const deadline = Date.now() + timeoutMs;
    await sleep(1000);   // a gadget restarted by the port opening boots first
    while (Date.now() < deadline) {
      if (want !== null) await write(`\n>speaker=${want ? 1 : 0}\n`);
      await write("\n>status\n");
      const wait = Date.now() + retryMs;
      while (Date.now() < wait) {
        if (heard !== null && (want === null || heard === want)) break;
        await sleep(100);
      }
      if (heard !== null && (want === null || heard === want)) break;
    }
  } finally {
    await reader.cancel().catch(() => {});
    await reading;
    reader.releaseLock();
    await port.close().catch(() => {});
  }

  if (heard === null) {
    throw new Error(
      "The gadget didn't answer. Check it runs firmware from this page and you picked its console port. " +
        "If it still doesn't answer, unplug it, plug it back in and try again.",
    );
  }
  if (want !== null && heard !== want) {
    throw new Error(`The gadget still reports the speaker ${heard ? "on" : "off"}. Try again.`);
  }
  return heard;
}

// Asks the gadget to play its built-in MP3 self-test (the console's "m"
// key, esp32/components/muse/muse_input.c): a fully local decode through
// the same audio write path the chat replies take, with no network in
// between — so a silent test means the fault is in the gadget's own output
// stage, and an audible one clears it. Nothing answers on the console, so
// the listener is the confirmation; the key is sent twice in case the
// first landed while the gadget was still booting after the port opened.
export async function gadgetTestSound(port) {
  try {
    await port.open({ baudRate: 115200 });
  } catch (e) {
    throw new Error(`Couldn't open the port (${e.message}). Close the installer's dialog first.`);
  }
  await port.setSignals({ dataTerminalReady: false, requestToSend: false }).catch(() => {});
  try {
    await sleep(1000);   // a gadget restarted by the port opening boots first
    for (let i = 0; i < 2; i++) {
      const writer = port.writable.getWriter();
      try {
        await writer.write(new TextEncoder().encode("m"));
      } finally {
        writer.releaseLock();
      }
      if (i == 0) await sleep(2500);
    }
  } finally {
    await port.close().catch(() => {});
  }
}
