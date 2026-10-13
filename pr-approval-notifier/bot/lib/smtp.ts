import net from "node:net";
import tls from "node:tls";

import { DEFAULT_EMAIL } from "./pending.js";

export type SmtpConfig = {
  host: string;
  port: number;
  user: string;
  password: string;
  from: string;
  to: string;
  starttls: boolean;
};

export type OutboundMail = {
  subject: string;
  text: string;
};

type MailSocket = net.Socket;

export function smtpConfigFromEnv(
  env: NodeJS.ProcessEnv = process.env,
): SmtpConfig | undefined {
  const host = env.APPROVAL_SMTP_HOST?.trim();
  const user = env.APPROVAL_SMTP_USER?.trim();
  const password = env.APPROVAL_SMTP_PASSWORD;
  const from = env.APPROVAL_SMTP_FROM?.trim();
  if (
    host === undefined ||
    host.length === 0 ||
    user === undefined ||
    user.length === 0 ||
    password === undefined ||
    password.length === 0 ||
    from === undefined ||
    from.length === 0
  ) {
    return undefined;
  }
  const port = Number(env.APPROVAL_SMTP_PORT?.trim() || "587");
  if (!Number.isInteger(port) || port <= 0) {
    throw new Error("APPROVAL_SMTP_PORT must be a positive integer");
  }
  const starttls =
    env.APPROVAL_SMTP_STARTTLS === undefined
      ? port === 587
      : env.APPROVAL_SMTP_STARTTLS.trim() !== "false";
  return {
    host,
    port,
    user,
    password,
    from,
    to: env.APPROVAL_EMAIL_TO?.trim() || DEFAULT_EMAIL,
    starttls: port === 465 ? false : starttls,
  };
}

class ReplyReader {
  private buffer = "";
  private pending: (() => void) | undefined;

  constructor(socket: MailSocket) {
    socket.setEncoding("utf8");
    socket.on("data", (chunk: string) => {
      this.buffer += chunk;
      const wake = this.pending;
      this.pending = undefined;
      wake?.();
    });
  }

  private waitForLine(): Promise<void> {
    if (this.buffer.includes("\r\n")) {
      return Promise.resolve();
    }
    return new Promise((resolve) => {
      this.pending = () => {
        this.pending = undefined;
        resolve();
      };
      if (this.buffer.includes("\r\n")) {
        const wake = this.pending;
        this.pending = undefined;
        wake?.();
      }
    });
  }

  async read(): Promise<{ code: number; lines: string[] }> {
    const lines: string[] = [];
    for (;;) {
      await this.waitForLine();
      let newline = this.buffer.indexOf("\r\n");
      while (newline >= 0) {
        const line = this.buffer.slice(0, newline);
        this.buffer = this.buffer.slice(newline + 2);
        lines.push(line);
        if (/^\d{3} /.test(line)) {
          return { code: Number(line.slice(0, 3)), lines };
        }
        newline = this.buffer.indexOf("\r\n");
      }
    }
  }
}

async function expectCode(
  reader: ReplyReader,
  codes: readonly number[],
  step: string,
): Promise<void> {
  const reply = await reader.read();
  if (!codes.includes(reply.code)) {
    throw new Error(
      `SMTP ${step} failed (${reply.code}): ${reply.lines.join(" | ")}`,
    );
  }
}

function command(socket: MailSocket, line: string): void {
  socket.write(`${line}\r\n`);
}

function dotStuff(text: string): string {
  const normalized = text.replace(/\r?\n/g, "\r\n");
  return normalized
    .split("\r\n")
    .map((line) => (line.startsWith(".") ? `.${line}` : line))
    .join("\r\n");
}

function encodeSubject(subject: string): string {
  if (/^[\t\x20-\x7e]*$/.test(subject)) {
    return subject;
  }
  return `=?UTF-8?B?${Buffer.from(subject, "utf8").toString("base64")}?=`;
}

function openSocket(config: SmtpConfig): MailSocket {
  const socket =
    config.port === 465
      ? tls.connect({
          host: config.host,
          port: config.port,
          servername: config.host,
        })
      : net.connect({ host: config.host, port: config.port });
  socket.setTimeout(20_000);
  socket.on("timeout", () => {
    socket.destroy(new Error("SMTP timeout"));
  });
  return socket;
}

async function waitConnected(
  socket: MailSocket,
  config: SmtpConfig,
): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    const onError = (error: Error) => {
      socket.off("error", onError);
      reject(error);
    };
    socket.once("error", onError);
    socket.once(config.port === 465 ? "secureConnect" : "connect", () => {
      socket.off("error", onError);
      resolve();
    });
  });
}

async function upgradeTls(
  socket: MailSocket,
  host: string,
): Promise<tls.TLSSocket> {
  socket.removeAllListeners("data");
  const secure = tls.connect({ socket, servername: host });
  await new Promise<void>((resolve, reject) => {
    secure.once("secureConnect", () => resolve());
    secure.once("error", reject);
  });
  return secure;
}

export async function sendMail(
  config: SmtpConfig,
  mail: OutboundMail,
): Promise<void> {
  let socket = openSocket(config);
  const readerAtStart = new ReplyReader(socket);
  try {
    await waitConnected(socket, config);
    let reader = readerAtStart;
    await expectCode(reader, [220], "greeting");
    command(socket, "EHLO pr-approval-notifier");
    await expectCode(reader, [250], "ehlo");
    if (config.starttls) {
      command(socket, "STARTTLS");
      await expectCode(reader, [220], "starttls");
      socket = await upgradeTls(socket, config.host);
      reader = new ReplyReader(socket);
      command(socket, "EHLO pr-approval-notifier");
      await expectCode(reader, [250], "ehlo");
    }
    command(socket, "AUTH LOGIN");
    await expectCode(reader, [334], "auth");
    command(socket, Buffer.from(config.user, "utf8").toString("base64"));
    await expectCode(reader, [334], "auth user");
    command(socket, Buffer.from(config.password, "utf8").toString("base64"));
    await expectCode(reader, [235], "auth password");
    command(socket, `MAIL FROM:<${config.from}>`);
    await expectCode(reader, [250], "mail from");
    command(socket, `RCPT TO:<${config.to}>`);
    await expectCode(reader, [250, 251], "rcpt to");
    command(socket, "DATA");
    await expectCode(reader, [354], "data");
    const payload = [
      `From: ${config.from}`,
      `To: ${config.to}`,
      `Subject: ${encodeSubject(mail.subject)}`,
      `Date: ${new Date().toUTCString()}`,
      "MIME-Version: 1.0",
      "Content-Type: text/plain; charset=utf-8",
      "Content-Transfer-Encoding: 8bit",
      "",
      dotStuff(mail.text),
      ".",
    ].join("\r\n");
    socket.write(`${payload}\r\n`);
    await expectCode(reader, [250], "queued");
    command(socket, "QUIT");
  } finally {
    socket.end();
  }
}
