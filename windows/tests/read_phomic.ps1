# Prints the texts in Phomic's window (UI Automation), from the status card to the pairing row.
param([switch]$All)
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$p = Get-Process Phomic -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "Phomic not running"; return }
$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $p.Id)
$win = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
if (-not $win) { "window hidden"; return }
$items = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
$lines = foreach ($t in $items) {
    $n = $t.Current.Name
    if ($n) { "{0}: {1}" -f $t.Current.ControlType.ProgrammaticName.Replace('ControlType.', ''), $n }
}
if ($All) { $lines } else {
    $start = 0
    $end = [Array]::FindIndex([string[]]$lines, [Predicate[string]] { param($l) $l -like '*Require a pairing code*' })
    if ($end -lt 0) { $end = [Math]::Min(30, $lines.Count) }
    $lines[$start..($end - 1)]
}
