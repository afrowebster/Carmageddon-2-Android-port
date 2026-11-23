"""Simple window for the APK builder: pick your installer, press Build."""
import os, queue, sys, threading, traceback
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import c2build


def main():
    root = tk.Tk()
    root.title('Carmageddon 2 Android port - APK builder %s' % c2build.VERSION)
    root.minsize(640, 460)
    q = queue.Queue()
    state = {'builder': None, 'thread': None}

    frm = ttk.Frame(root, padding=12)
    frm.pack(fill='both', expand=True)
    frm.columnconfigure(1, weight=1)

    intro = ('Builds an Android APK from your own copy of Carmageddon II (GOG release, '
             'setup_carmageddon2_2.1.0.28.exe). The APK contains your game files: keep it for yourself.')
    ttk.Label(frm, text=intro, wraplength=600, justify='left').grid(row=0, column=0, columnspan=3, sticky='w', pady=(0, 10))

    src = tk.StringVar()
    out = tk.StringVar(value=os.path.join(os.path.expanduser('~'), 'carmageddon2.apk'))

    def pick_src():
        p = filedialog.askopenfilename(title='GOG installer', filetypes=[('Installer', '*.exe'), ('All files', '*')])
        if p:
            src.set(p)

    def pick_dir():
        p = filedialog.askdirectory(title='Installed game folder (with CARMA2_HW.EXE)')
        if p:
            src.set(p)

    def pick_out():
        p = filedialog.asksaveasfilename(title='Save APK as', defaultextension='.apk',
                                         initialfile=os.path.basename(out.get()), filetypes=[('Android app', '*.apk')])
        if p:
            out.set(p)

    ttk.Label(frm, text='Game:').grid(row=1, column=0, sticky='w')
    ttk.Entry(frm, textvariable=src).grid(row=1, column=1, sticky='ew', padx=6)
    b = ttk.Frame(frm)
    b.grid(row=1, column=2, sticky='e')
    ttk.Button(b, text='Installer...', command=pick_src).pack(side='left')
    ttk.Button(b, text='Folder...', command=pick_dir).pack(side='left', padx=(4, 0))

    ttk.Label(frm, text='Save as:').grid(row=2, column=0, sticky='w', pady=6)
    ttk.Entry(frm, textvariable=out).grid(row=2, column=1, sticky='ew', padx=6)
    ttk.Button(frm, text='Browse...', command=pick_out).grid(row=2, column=2, sticky='e')

    x86 = tk.BooleanVar(value=False)
    ttk.Checkbutton(frm, text='Also include x86_64 (for PC emulators; makes the build twice as long)',
                    variable=x86).grid(row=3, column=1, columnspan=2, sticky='w')

    bar = ttk.Progressbar(frm, maximum=1000)
    bar.grid(row=4, column=0, columnspan=3, sticky='ew', pady=(12, 2))
    status = tk.StringVar(value='Ready')
    ttk.Label(frm, textvariable=status).grid(row=5, column=0, columnspan=2, sticky='w')
    go = ttk.Button(frm, text='Build APK')
    go.grid(row=5, column=2, sticky='e')

    logbox = tk.Text(frm, height=14, wrap='word', state='disabled')
    logbox.grid(row=6, column=0, columnspan=3, sticky='nsew', pady=(8, 0))
    frm.rowconfigure(6, weight=1)

    def log(msg):
        q.put(('log', msg))

    def progress(frac, text):
        q.put(('progress', (frac, text)))

    def work(bld):
        try:
            bld.build()
            q.put(('done', None))
        except c2build.BuildError as e:
            q.put(('error', str(e)))
        except Exception:
            q.put(('error', traceback.format_exc()))

    def start():
        if state['thread']:
            if messagebox.askyesno('Cancel', 'Stop the build?'):
                state['builder'].cancelled = True
                status.set('Stopping...')
            return
        if not src.get():
            messagebox.showinfo('Game', 'Choose the GOG installer (setup_carmageddon2_2.1.0.28.exe) or the installed game folder.')
            return
        abis = ['arm64-v8a'] + (['x86_64'] if x86.get() else [])
        bld = c2build.Builder(src.get(), out.get(), abis, log=log, progress=progress)
        state['builder'] = bld
        state['thread'] = threading.Thread(target=work, args=(bld,), daemon=True)
        go.config(text='Cancel')
        state['thread'].start()

    go.config(command=start)

    def poll():
        try:
            while True:
                kind, v = q.get_nowait()
                if kind == 'log':
                    logbox.config(state='normal')
                    logbox.insert('end', v + '\n')
                    logbox.see('end')
                    logbox.config(state='disabled')
                elif kind == 'progress':
                    bar['value'] = int(v[0] * 1000)
                    status.set(v[1])
                else:
                    state['thread'] = None
                    go.config(text='Build APK')
                    if kind == 'done':
                        status.set('Done')
                        messagebox.showinfo('Done', 'APK ready:\n%s' % out.get())
                    else:
                        status.set('Failed')
                        log('ERROR: ' + v)
                        messagebox.showerror('Build failed', v[-1500:])
        except queue.Empty:
            pass
        root.after(100, poll)

    poll()
    root.mainloop()


if __name__ == '__main__':
    main()
