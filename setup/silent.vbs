' Start the setup GUI with no console window.
Option Explicit
Dim sh, fso, here, root, args, i, py
Set sh = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
here = fso.GetParentFolderName(WScript.ScriptFullName)
root = fso.GetParentFolderName(here)
sh.CurrentDirectory = root

args = ""
For i = 0 To WScript.Arguments.Count - 1
  args = args & " " & Quote(WScript.Arguments(i))
Next

py = FindPy()
If py = "" Then
  sh.Run "cmd /c echo Python 3 is needed to open the setup window. & pause", 1, True
  WScript.Quit 1
End If
sh.Run py & " -B setup\helix_setup.py" & args, 0, False

Function Quote(s)
  If InStr(s, " ") > 0 Then Quote = """" & s & """" Else Quote = s
End Function

Function Have(name)
  Have = (sh.Run("cmd /c where " & name & " >nul 2>&1", 0, True) = 0)
End Function

Function FindPy()
  If Have("pythonw") Then FindPy = "pythonw": Exit Function
  If Have("pyw") Then FindPy = "pyw -3": Exit Function
  If Have("python") Then FindPy = "python": Exit Function
  If Have("py") Then FindPy = "py -3": Exit Function
  FindPy = ""
End Function
