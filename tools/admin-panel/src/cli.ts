import { openDb } from './db.js';
import { addUser, removeUser, resetUser } from './users.js';

async function main(): Promise<number> {
  const [cmd, name] = process.argv.slice(2);
  if (!['add-user', 'remove-user', 'reset-user'].includes(cmd ?? '') || !name) {
    console.error('usage: node dist/cli.js add-user|remove-user|reset-user <name>');
    return 2;
  }
  const db = openDb(process.env.ADMIN_DB || '/data/admin.db');
  try {
    if (cmd === 'remove-user') {
      if (!removeUser(db, name)) { console.error(`no such user: ${name}`); return 1; }
      console.log(`removed ${name}`);
      return 0;
    }
    const c = cmd === 'add-user' ? await addUser(db, name) : await resetUser(db, name);
    // Shown once: only the hash and secret are stored, and the secret is only needed to enrol.
    console.log(`password: ${c.password}`);
    console.log(`otpauth:  ${c.otpauth}`);
    console.log('The password must be changed at first sign-in. These are not shown again.');
    return 0;
  } catch (e) {
    console.error((e as Error).message);
    return 1;
  } finally {
    db.close();
  }
}

main().then((code) => { process.exitCode = code; });
