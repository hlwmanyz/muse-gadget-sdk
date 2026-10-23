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

// Reads and sets the gadget's speaker switch over Web Serial, the same way
// sdk-token.js delivers the token: ">" setup commands on the console port
// (esp32/components/muse/muse_input.c, muse_ble.c). The gadget's own
// "@status" reply confirms the state, so the checkbox on the page only ever
// shows what the gadget itself reports — never a guess.

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

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
