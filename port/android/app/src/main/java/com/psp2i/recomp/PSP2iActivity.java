package com.psp2i.recomp;

import android.Manifest;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.Settings;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.concurrent.CountDownLatch;

import org.libsdl.app.SDLActivity;

/* SDL's activity runs main() from libmain.so (port/host/sdl_main.c), which
 * calls the two static helpers below from the game's thread:
 *
 *   openGameImage  the game's ISO, picked with Android's file picker once and
 *                  remembered (a persistable read grant: no storage permission)
 *   copyFonts      the PSP fonts, copied once from the flash/ folder of a
 *                  GameData folder the user picks, into the data folder
 *   dataFolder     where psp2i.ini, GameData (saves, fonts) and logs/ live: a
 *                  folder the player chooses at the first start (and can move
 *                  with the multiplayer dialog's "Data folder" button), so they
 *                  can open the files with any file manager or over USB (not
 *                  the app-private Android/data folder, which Android 11+ hides)
 *
 * Both show their dialog on the UI thread and wait for the answer. */
public class PSP2iActivity extends SDLActivity {
    private static final int REQ_ISO = 0x5101, REQ_FOLDER = 0x5102, REQ_STORAGE = 0x5103, REQ_ALL_FILES = 0x5104;
    private static PSP2iActivity sSelf;
    private static CountDownLatch sLatch;
    private static Uri sPicked;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sSelf = this;
        super.onCreate(savedInstanceState);
    }

    /* Explain, open the picker, and wait for what the user chose (null: declined). */
    private static Uri ask(final String title, final String message, final boolean folder) {
        final PSP2iActivity a = sSelf;
        if (a == null) return null;
        final CountDownLatch latch = new CountDownLatch(1);
        sLatch = latch;
        sPicked = null;
        a.runOnUiThread(() -> new AlertDialog.Builder(a)
            .setTitle(title)
            .setMessage(message)
            .setCancelable(false)
            .setPositiveButton("Choose", (d, w) -> {
                Intent i = new Intent(folder ? Intent.ACTION_OPEN_DOCUMENT_TREE : Intent.ACTION_OPEN_DOCUMENT);
                if (!folder) {
                    i.addCategory(Intent.CATEGORY_OPENABLE);
                    i.setType("*/*");
                }
                try {
                    a.startActivityForResult(i, folder ? REQ_FOLDER : REQ_ISO);
                } catch (Exception e) {
                    latch.countDown();
                }
            })
            .setNegativeButton("Quit", (d, w) -> latch.countDown())
            .show());
        try {
            latch.await();
        } catch (InterruptedException e) {
            return null;
        }
        return sPicked;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQ_ALL_FILES) {                 // back from the "All files access" screen
            if (sLatch != null) sLatch.countDown();
            return;
        }
        if (requestCode == REQ_ISO || requestCode == REQ_FOLDER) {
            if (resultCode == RESULT_OK && data != null) sPicked = data.getData();
            if (sLatch != null) sLatch.countDown();
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode == REQ_STORAGE) {
            if (sLatch != null) sLatch.countDown();
            return;
        }
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
    }

    /* ---- the data folder ------------------------------------------------------------- */

    private static String sData;

    private static boolean haveStorage(PSP2iActivity a) {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return a.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    /* Explain, ask for the storage permission (Android 11+: the "All files
     * access" screen; before: the runtime permission), wait, and say whether
     * it is now granted. */
    private static boolean askStorage(final PSP2iActivity a) {
        final CountDownLatch latch = new CountDownLatch(1);
        sLatch = latch;
        a.runOnUiThread(() -> new AlertDialog.Builder(a)
            .setTitle("Storage")
            .setMessage("PSP2i keeps its settings (psp2i.ini), saves, fonts and logs in a folder you choose, "
                        + "so you can open them with a file manager or over USB. Android needs "
                        + (Build.VERSION.SDK_INT >= 30 ? "\"All files access\" for this; turn it on for PSP2i on the next screen."
                                                       : "the storage permission for this."))
            .setCancelable(false)
            .setPositiveButton("Allow", (d, w) -> {
                try {
                    if (Build.VERSION.SDK_INT >= 30) {
                        Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                              Uri.parse("package:" + a.getPackageName()));
                        a.startActivityForResult(i, REQ_ALL_FILES);
                    } else {
                        a.requestPermissions(new String[] { Manifest.permission.WRITE_EXTERNAL_STORAGE,
                                                            Manifest.permission.READ_EXTERNAL_STORAGE }, REQ_STORAGE);
                    }
                } catch (Exception e) {
                    latch.countDown();
                }
            })
            .setNegativeButton("Not now", (d, w) -> latch.countDown())
            .show());
        try {
            latch.await();
        } catch (InterruptedException e) {
            return false;
        }
        return haveStorage(a);
    }

    /* Files of src that dst lacks, copied (the originals stay). */
    private static void copyMissing(File src, File dst) {
        File[] list = src.listFiles();
        if (list == null) return;
        for (File f : list) {
            File out = new File(dst, f.getName());
            if (f.isDirectory()) {
                if (out.isDirectory() || out.mkdirs()) copyMissing(f, out);
            } else if (!out.exists()) {
                try (InputStream in = new FileInputStream(f); OutputStream o = new FileOutputStream(out)) {
                    byte[] buf = new byte[1 << 16];
                    int n;
                    while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
                } catch (Exception e) {
                    out.delete();                       // no half-copied file
                }
            }
        }
    }

    /* A message with one button; waits until it is closed. */
    private static void tell(final PSP2iActivity a, final String title, final String message) {
        final CountDownLatch latch = new CountDownLatch(1);
        a.runOnUiThread(() -> new AlertDialog.Builder(a)
            .setTitle(title)
            .setMessage(message)
            .setCancelable(false)
            .setPositiveButton("OK", (d, w) -> latch.countDown())
            .show());
        try {
            latch.await();
        } catch (InterruptedException e) {
            // go on
        }
    }

    /* The file path of a folder from the folder picker: internal storage
     * ("primary:<path>"), an SD card ("<volume id>:<path>") or a raw path. */
    private static String treePath(Uri tree) {
        final String id;
        try {
            id = DocumentsContract.getTreeDocumentId(tree);
        } catch (Exception e) {
            return null;
        }
        final int colon = id.indexOf(':');
        if (colon < 0) return null;
        final String vol = id.substring(0, colon), rel = id.substring(colon + 1);
        if ("raw".equalsIgnoreCase(vol)) return rel;
        final File root = "primary".equalsIgnoreCase(vol) ? Environment.getExternalStorageDirectory() : new File("/storage/" + vol);
        return rel.isEmpty() ? root.getAbsolutePath() : new File(root, rel).getAbsolutePath();
    }

    /* Whether the game can make and write files in this folder itself (the
     * native code opens them by path). */
    private static boolean writable(String path) {
        final File dir = new File(path);
        if (!dir.isDirectory() && !dir.mkdirs()) return false;
        final File probe = new File(dir, ".psp2i_write_test");
        try (OutputStream o = new FileOutputStream(probe)) {
            o.write(1);
        } catch (Exception e) {
            return false;
        }
        probe.delete();
        return true;
    }

    /* The player picks the data folder: storage access first (Android 11+:
     * "All files access", which the game needs to use a folder of its own
     * choosing), then the folder. Asked until a usable one is picked; null
     * if the player quits. */
    private static String chooseDataFolder(final PSP2iActivity a, final String why) {
        for (;;) {
            if (!haveStorage(a) && !askStorage(a)) {
                if (!askRetry(a, "Storage", "PSP2i needs storage access to keep its files in a folder you choose. "
                                            + "Without it, it cannot start.")) return null;
                continue;
            }
            final Uri tree = ask("Data folder", why + "\n\nPSP2i keeps its settings (psp2i.ini), saves, fonts and "
                                 + "logs there, where you can open them with a file manager or over USB. Make or pick "
                                 + "a folder of its own, e.g. PSP2i in your internal storage.", true);
            if (tree == null) return null;
            final String path = treePath(tree);
            if (path != null && writable(path)) return path;
            if (!askRetry(a, "Data folder", "PSP2i cannot write to that folder" + (path != null ? " (" + path + ")" : "")
                                            + ". Pick a folder in your internal storage or on an SD card "
                                            + "(not the top of the storage or Downloads itself).")) return null;
        }
    }

    /* A message with Retry / Quit; true for Retry. */
    private static boolean askRetry(final PSP2iActivity a, final String title, final String message) {
        final boolean[] retry = { false };
        final CountDownLatch latch = new CountDownLatch(1);
        a.runOnUiThread(() -> new AlertDialog.Builder(a)
            .setTitle(title)
            .setMessage(message)
            .setCancelable(false)
            .setPositiveButton("Retry", (d, w) -> { retry[0] = true; latch.countDown(); })
            .setNegativeButton("Quit", (d, w) -> latch.countDown())
            .show());
        try {
            latch.await();
        } catch (InterruptedException e) {
            return false;
        }
        return retry[0];
    }

    /* Files the earlier data folders hold, copied into `to` where it lacks
     * them (the originals stay): the app-private folder of the first
     * versions and the fixed PSP2i/ folder of the last one. */
    private static void copyOld(final PSP2iActivity a, final File to, final String also) {
        final File priv = a.getExternalFilesDir(null);
        final File[] old = { priv, new File(Environment.getExternalStorageDirectory(), "PSP2i"),
                             also != null ? new File(also) : null };
        for (File f : old) copyFolder(f, to);
    }

    /* copyMissing, unless the two are the same folder or one is inside the
     * other (the copy would then copy itself). */
    private static void copyFolder(File from, File to) {
        if (from == null || !from.isDirectory()) return;
        final String f = from.getAbsolutePath() + "/", t = to.getAbsolutePath() + "/";
        if (f.startsWith(t) || t.startsWith(f)) return;
        copyMissing(from, to);
    }

    private static final String PREF_DATA = "data_folder";

    /* The data folder (see the top): the one the player chose. The first
     * time -- or when it is gone or access was taken away -- the player
     * chooses it, and the files of the earlier folders are copied in. Null if
     * the player quits; the app then closes. */
    public static String dataFolder() {
        if (sData != null) return sData;
        final PSP2iActivity a = sSelf;
        if (a == null) return null;
        final SharedPreferences prefs = a.getSharedPreferences("psp2i", MODE_PRIVATE);
        final String saved = prefs.getString(PREF_DATA, null);
        if (saved != null && haveStorage(a) && new File(saved).isDirectory()) {
            sData = saved;
            return sData;
        }
        final String path = chooseDataFolder(a, saved == null
            ? "Choose where PSP2i keeps its files."
            : "PSP2i's data folder (" + saved + ") cannot be used any more. Choose it again, or another one.");
        if (path == null) {
            a.runOnUiThread(a::finishAndRemoveTask);
            return null;
        }
        copyOld(a, new File(path), saved);
        prefs.edit().putString(PREF_DATA, path).apply();
        sData = path;
        return sData;
    }

    /* Move the data folder: the player picks the new one, the files are copied
     * there (the old folder is left as it was) and it is used from the next
     * start. PSP2i then closes, before the game has started. */
    private static void moveDataFolder(final PSP2iActivity a) {
        final String path = chooseDataFolder(a, "Choose the new folder for PSP2i's files"
                                                + (sData != null ? " (now " + sData + ")" : "") + ".");
        if (path == null || path.equals(sData)) return;
        if (sData != null && ((path + "/").startsWith(sData + "/") || (sData + "/").startsWith(path + "/"))) {
            tell(a, "Data folder", "The new folder cannot be inside the current one (" + sData + ") or contain it. "
                                   + "Nothing was moved.");
            return;
        }
        if (sData != null) copyMissing(new File(sData), new File(path));
        copyOld(a, new File(path), null);
        a.getSharedPreferences("psp2i", MODE_PRIVATE).edit().putString(PREF_DATA, path).commit();
        tell(a, "Data folder", "PSP2i's files are now in\n\n" + path + "\n\nThe old folder"
                               + (sData != null ? " (" + sData + ")" : "") + " was left as it was; delete it once "
                               + "you have checked the new one. PSP2i closes now: start it again to use the new folder.");
        a.runOnUiThread(a::finishAndRemoveTask);
        System.exit(0);
    }

    /* The modern multiplayer server: "host" (this phone hosts) or the hosting
     * player's address. Asked at every start, prefilled with the last answer;
     * `here` is this device's address, for the players who join it. The
     * dialog's "Data folder" button moves PSP2i's files (moveDataFolder). */
    public static String askModernServer(final String here) {
        final PSP2iActivity a = sSelf;
        if (a == null) return null;
        for (;;) {
            final boolean[] move = { false };
            final String server = askModernServerOnce(a, here, move);
            if (!move[0]) return server;
            moveDataFolder(a);                    // closes the app when it moved; else ask again
        }
    }

    private static String askModernServerOnce(final PSP2iActivity a, final String here, final boolean[] move) {
        final SharedPreferences prefs = a.getSharedPreferences("psp2i", MODE_PRIVATE);
        final String last = prefs.getString("modern_server", "host");
        final String[] answer = { last };
        final CountDownLatch latch = new CountDownLatch(1);
        a.runOnUiThread(() -> {
            final android.widget.EditText edit = new android.widget.EditText(a);
            edit.setSingleLine(true);
            edit.setText(last);
            edit.setSelection(last.length());
            new AlertDialog.Builder(a)
                .setTitle("Multiplayer server")
                .setMessage("Ad hoc multiplayer (modern): type host to host on this device, or the address of the "
                            + "player hosting (shown in their settings menu under MODERN SERVER)."
                            + (here != null && !here.isEmpty()
                               ? "\n\nTo host here, the others type: " + here
                                 + "\n(players outside this network also need TCP 27330-27331 forwarded to this device)"
                               : ""))
                .setView(edit)
                .setCancelable(false)
                .setPositiveButton("OK", (d, w) -> {
                    String t = edit.getText().toString().trim();
                    answer[0] = t.isEmpty() ? "host" : t;
                    prefs.edit().putString("modern_server", answer[0]).apply();
                    latch.countDown();
                })
                .setNeutralButton("Data folder", (d, w) -> { move[0] = true; latch.countDown(); })
                .show();
        });
        try {
            latch.await();
        } catch (InterruptedException e) {
            return last;
        }
        return answer[0];
    }

    /* A one-line text dialog for the settings menu's server items: the text
     * typed (OK), or null (Cancel). Called from the game's thread. */
    public static String askText(final String title, final String message, final String value) {
        final PSP2iActivity a = sSelf;
        if (a == null) return null;
        final String[] answer = { null };
        final CountDownLatch latch = new CountDownLatch(1);
        a.runOnUiThread(() -> {
            final android.widget.EditText edit = new android.widget.EditText(a);
            edit.setSingleLine(true);
            edit.setText(value != null ? value : "");
            edit.setSelection(edit.getText().length());
            new AlertDialog.Builder(a)
                .setTitle(title)
                .setMessage(message)
                .setView(edit)
                .setCancelable(false)
                .setPositiveButton("OK", (d, w) -> { answer[0] = edit.getText().toString().trim(); latch.countDown(); })
                .setNegativeButton("Cancel", (d, w) -> latch.countDown())
                .show();
        });
        try {
            latch.await();
        } catch (InterruptedException e) {
            return null;
        }
        return answer[0];
    }

    /* A file descriptor for the game's ISO (the caller owns it), or -1. */
    public static int openGameImage(boolean choose) {
        final PSP2iActivity a = sSelf;
        if (a == null) return -1;
        SharedPreferences prefs = a.getSharedPreferences("psp2i", MODE_PRIVATE);
        String saved = prefs.getString("iso", null);
        Uri uri = !choose && saved != null ? Uri.parse(saved) : null;
        for (int attempt = 0; attempt < 2; attempt++) {
            if (uri == null) {
                uri = ask("Game ISO", "Choose the game's disc image (.iso) of Phantasy Star Portable 2 Infinity.", false);
                if (uri == null) return -1;
                try {
                    a.getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (Exception e) {
                    // not persistable from this provider: it is asked for again next time
                }
                prefs.edit().putString("iso", uri.toString()).apply();
            }
            try {
                ParcelFileDescriptor pfd = a.getContentResolver().openFileDescriptor(uri, "r");
                if (pfd != null) return pfd.detachFd();
            } catch (Exception e) {
                // moved or deleted since: ask again
            }
            uri = null;
        }
        return -1;
    }

    /* Copy flash/ from a GameData folder the user picks to <files>/GameData/flash. */
    public static boolean copyFonts() {
        final PSP2iActivity a = sSelf;
        if (a == null) return false;
        Uri tree = ask("PSP fonts",
            "Choose your GameData folder (or its flash folder). The PSP fonts in flash/ are copied into the app once.", true);
        if (tree == null) return false;
        ContentResolver cr = a.getContentResolver();
        String rootId = DocumentsContract.getTreeDocumentId(tree);
        String flashId = rootId;
        if (findChild(cr, tree, rootId, "font") == null) {      // GameData was picked, not flash itself
            flashId = findChild(cr, tree, rootId, "flash");
            if (flashId == null) return false;
        }
        String base = dataFolder();
        File dst = new File(base != null ? new File(base) : a.getExternalFilesDir(null), "GameData/flash");
        if (!dst.isDirectory() && !dst.mkdirs()) return false;
        return copyTree(cr, tree, flashId, dst);
    }

    private static final String[] COLUMNS = {
        Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE
    };

    private static String findChild(ContentResolver cr, Uri tree, String parentId, String name) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        try (Cursor c = cr.query(children, COLUMNS, null, null, null)) {
            while (c != null && c.moveToNext())
                if (name.equalsIgnoreCase(c.getString(1)) && Document.MIME_TYPE_DIR.equals(c.getString(2)))
                    return c.getString(0);
        } catch (Exception e) {
            return null;
        }
        return null;
    }

    private static boolean copyTree(ContentResolver cr, Uri tree, String parentId, File dst) {
        boolean any = false;
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        try (Cursor c = cr.query(children, COLUMNS, null, null, null)) {
            while (c != null && c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                File out = new File(dst, name);
                if (Document.MIME_TYPE_DIR.equals(mime)) {
                    if (out.isDirectory() || out.mkdirs()) any |= copyTree(cr, tree, id, out);
                    continue;
                }
                try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, id));
                     OutputStream o = new FileOutputStream(out)) {
                    byte[] buf = new byte[1 << 16];
                    int n;
                    while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
                    any = true;
                }
            }
        } catch (Exception e) {
            return any;
        }
        return any;
    }
}
