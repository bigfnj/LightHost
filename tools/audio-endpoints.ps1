# Lists every active audio endpoint and which default roles it holds.
#
# Why roles matter: Windows keeps THREE separate defaults per direction --
# Console, Multimedia and Communications. Teams and most softphones follow the
# Communications role, so a chain can be wired perfectly and still be bypassed
# because the calling app is reading a different device than the one you set.
#
# And the reverse hazard: if the default RENDER device is the virtual cable that
# the host writes into, then all system audio -- including the far end of a call
# -- lands in the cable and is sent back as your microphone. That is heard by
# everyone else as feedback.
$src = @'
using System;
using System.Runtime.InteropServices;

public static class Endpoints
{
    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
    internal class EnumeratorComObject { }

    // [PreserveSig] on every method, and it is load-bearing rather than tidy.
    //
    // These are all declared returning `int`, i.e. the raw HRESULT, and the
    // callers below test it. Without [PreserveSig] the CLR's COM marshaller
    // takes that HRESULT away: a failing call raises a COMException instead of
    // returning, so `if (... == 0)` is never reached and the guard cannot fire.
    //
    // That is not hypothetical. Running this on an RDP session, where a role
    // legitimately has no default endpoint, produced
    //
    //     Exception calling "Report": "Element not found. (0x80070490)"
    //
    // from a script whose next line was written to handle exactly that case. It
    // is the same shape as the five unreachable gates fixed in 5.3.0: a check
    // that looks correct and cannot run.
    [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDeviceEnumerator
    {
        [PreserveSig] int EnumAudioEndpoints(int dataFlow, int stateMask, out IMMDeviceCollection devices);
        [PreserveSig] int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice device);
    }

    [Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDeviceCollection
    {
        [PreserveSig] int GetCount(out int count);
        [PreserveSig] int Item(int index, out IMMDevice device);
    }

    [Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDevice
    {
        [PreserveSig] int Activate(ref Guid iid, int clsCtx, IntPtr activationParams,
                                   [MarshalAs(UnmanagedType.IUnknown)] out object iface);
        [PreserveSig] int OpenPropertyStore(int stgmAccess, out IPropertyStore properties);
        [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        [PreserveSig] int GetState(out int state);
    }

    [Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IPropertyStore
    {
        [PreserveSig] int GetCount(out int count);
        [PreserveSig] int GetAt(int index, out PropertyKey key);
        [PreserveSig] int GetValue(ref PropertyKey key, out PropVariant value);
        [PreserveSig] int SetValue(ref PropertyKey key, ref PropVariant value);
        [PreserveSig] int Commit();
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct PropertyKey { public Guid fmtid; public int pid; }

    [StructLayout(LayoutKind.Explicit)]
    internal struct PropVariant
    {
        [FieldOffset(0)] public short vt;
        [FieldOffset(8)] public IntPtr pointerValue;
    }

    // Both of these return a placeholder rather than throwing, and both check,
    // for the same reason the interfaces carry [PreserveSig]: a device that
    // cannot report its name is worth one odd-looking row in the output, not a
    // dead script. A missing property store used to be a NullReferenceException
    // on the next line.
    private static string NameOf(IMMDevice dev)
    {
        IPropertyStore store;

        if (dev.OpenPropertyStore(0, out store) != 0 || store == null)
            return "(name unavailable)";

        var key = new PropertyKey { fmtid = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), pid = 14 };
        PropVariant pv;

        if (store.GetValue(ref key, out pv) != 0)
            return "(name unavailable)";

        var name = Marshal.PtrToStringUni(pv.pointerValue);
        return string.IsNullOrEmpty(name) ? "(unnamed device)" : name;
    }

    private static string IdOf(IMMDevice dev)
    {
        string id;

        // Returns null, not a placeholder: this value is compared against the
        // default-role ids, and a placeholder would make two unreadable devices
        // compare equal and both get tagged as the default.
        if (dev.GetId(out id) != 0)
            return null;

        return id;
    }

    public static string Report()
    {
        var en = (IMMDeviceEnumerator)(new EnumeratorComObject());
        var sb = new System.Text.StringBuilder();
        var roles = new string[] { "Console", "Multimedia", "Communications" };

        for (int flow = 0; flow < 2; flow++)
        {
            sb.AppendLine(flow == 0 ? "=== PLAYBACK (render) ===" : "=== RECORDING (capture) ===");

            // Which device holds each default role.
            var defaults = new string[3];
            for (int r = 0; r < 3; r++)
            {
                IMMDevice d;
                if (en.GetDefaultAudioEndpoint(flow, r, out d) == 0 && d != null)
                    defaults[r] = IdOf(d);
            }

            // Checked, because [PreserveSig] means these now RETURN their
            // failure rather than throwing it. Before, an unchecked return was
            // survivable only because the marshaller raised instead; leaving
            // them unchecked afterwards would turn a reportable error into a
            // NullReferenceException on the next line.
            IMMDeviceCollection col;

            if (en.EnumAudioEndpoints(flow, 1, out col) != 0 || col == null)   // active only
            {
                sb.AppendLine("  (could not enumerate endpoints for this direction)");
                sb.AppendLine();
                continue;
            }

            int count;

            if (col.GetCount(out count) != 0)
            {
                sb.AppendLine("  (endpoint count unavailable)");
                sb.AppendLine();
                continue;
            }

            for (int i = 0; i < count; i++)
            {
                IMMDevice dev;

                if (col.Item(i, out dev) != 0 || dev == null)
                {
                    sb.AppendLine("  (endpoint " + i + " could not be read)");
                    continue;
                }

                var id = IdOf(dev);
                var tags = "";
                for (int r = 0; r < 3; r++)
                    if (defaults[r] == id) tags += " [" + roles[r] + "]";

                sb.AppendLine(string.Format("  {0,-52}{1}", NameOf(dev), tags));
            }
            sb.AppendLine();
        }
        return sb.ToString();
    }
}
'@
Add-Type -TypeDefinition $src -ErrorAction Stop
[Endpoints]::Report()
