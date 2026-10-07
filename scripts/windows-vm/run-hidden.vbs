' Runs a command line in the guest without a visible console window (prlctl exec of cmd.exe /
' powershell.exe opens a Windows Terminal window in the user's session, which would cover the
' desktop in screenshots). Waits and returns the command's exit code.
'   wscript.exe //B //Nologo run-hidden.vbs <program> [arguments...]
Dim sh, args, i, line
Set sh = CreateObject("WScript.Shell")
Set args = WScript.Arguments
line = ""
For i = 0 To args.Count - 1
  If InStr(args(i), " ") > 0 Then
    line = line & """" & args(i) & """ "
  Else
    line = line & args(i) & " "
  End If
Next
WScript.Quit sh.Run(line, 0, True)
