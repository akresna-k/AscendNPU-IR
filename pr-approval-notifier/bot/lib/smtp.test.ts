import assert from "node:assert/strict";
import net from "node:net";
import test from "node:test";

import { sendMail, smtpConfigFromEnv } from "./smtp.js";

test("smtp config is absent until the relay credentials are set", () => {
  assert.equal(
    smtpConfigFromEnv({
      APPROVAL_EMAIL_TO: "kusjantono.kresna.kirana@huawei.com",
    }),
    undefined,
  );
  const config = smtpConfigFromEnv({
    APPROVAL_SMTP_HOST: "smtp.example.com",
    APPROVAL_SMTP_USER: "notifier",
    APPROVAL_SMTP_PASSWORD: "secret",
    APPROVAL_SMTP_FROM: "notifier@example.com",
  });
  assert.equal(config?.to, "kusjantono.kresna.kirana@huawei.com");
  assert.equal(config?.port, 587);
  assert.equal(config?.starttls, true);
});

test("delivers a digest through AUTH LOGIN", async () => {
  const received: string[] = [];
  const server = net.createServer((socket) => {
    socket.write("220 localhost ESMTP\r\n");
    let buf = "";
    let mode: "cmd" | "data" = "cmd";
    let body = "";
    let auth: "user" | "pass" | undefined;

    const takeBody = () => {
      const end = body.indexOf("\r\n.\r\n");
      if (end < 0) {
        return;
      }
      received.push(body.slice(0, end));
      body = body.slice(end + 5);
      mode = "cmd";
      socket.write("250 queued\r\n");
    };

    socket.on("data", (chunk: Buffer) => {
      buf += chunk.toString("utf8");
      if (mode === "data") {
        body += buf;
        buf = "";
        takeBody();
        return;
      }
      let newline = buf.indexOf("\r\n");
      while (newline >= 0 && mode === "cmd") {
        const line = buf.slice(0, newline);
        buf = buf.slice(newline + 2);
        const upper = line.toUpperCase();
        if (upper.startsWith("EHLO") || upper.startsWith("HELO")) {
          socket.write("250-localhost\r\n250 AUTH LOGIN\r\n");
        } else if (upper === "AUTH LOGIN") {
          auth = "user";
          socket.write("334 VXNlcm5hbWU6\r\n");
        } else if (auth === "user") {
          assert.equal(
            Buffer.from(line, "base64").toString("utf8"),
            "notifier",
          );
          auth = "pass";
          socket.write("334 UGFzc3dvcmQ6\r\n");
        } else if (auth === "pass") {
          assert.equal(Buffer.from(line, "base64").toString("utf8"), "secret");
          auth = undefined;
          socket.write("235 ok\r\n");
        } else if (upper.startsWith("MAIL FROM")) {
          socket.write("250 ok\r\n");
        } else if (upper.startsWith("RCPT TO")) {
          assert.match(line, /<kusjantono\.kresna\.kirana@huawei\.com>/);
          socket.write("250 ok\r\n");
        } else if (upper === "DATA") {
          mode = "data";
          body = buf;
          buf = "";
          socket.write("354 go\r\n");
          takeBody();
        } else if (upper === "QUIT") {
          socket.write("221 bye\r\n");
          socket.end();
        } else {
          socket.write("500 unexpected\r\n");
        }
        newline = buf.indexOf("\r\n");
      }
    });
  });

  await new Promise<void>((resolve) => {
    server.listen(0, "127.0.0.1", () => resolve());
  });
  const address = server.address();
  const port = typeof address === "object" && address ? address.port : 0;
  try {
    await sendMail(
      {
        host: "127.0.0.1",
        port,
        user: "notifier",
        password: "secret",
        from: "notifier@example.com",
        to: "kusjantono.kresna.kirana@huawei.com",
        starttls: false,
      },
      {
        subject: "AscendNPU-IR: 1 pull request needs your approval",
        text: "See #3758\n.not-a-terminator",
      },
    );
  } finally {
    await new Promise<void>((resolve, reject) => {
      server.close((error) => (error ? reject(error) : resolve()));
    });
  }
  assert.equal(received.length, 1);
  assert.match(received[0] ?? "", /Subject: AscendNPU-IR: 1 pull request needs your approval/);
  assert.match(received[0] ?? "", /See #3758/);
  assert.match(received[0] ?? "", /\r\n\.\.not-a-terminator/);
});
