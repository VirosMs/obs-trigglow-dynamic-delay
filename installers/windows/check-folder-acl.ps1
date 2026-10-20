# Used by trigglow-dynamic-delay-setup.iss (SecurePluginsRoot), never
# installed: tells whether a folder can be written to by anyone but
# Administrators, SYSTEM and TrustedInstaller. Writes one line to -Out:
#   ok                 only those can write to it, and one of them owns it
#   fix: <reasons>     another owner, or Allow entries that let others write
#   error: <message>   the permissions could not be read
# SIDs, not names: the names are translated on a Spanish Windows.
param(
  [Parameter(Mandatory = $true)][string]$Path,
  [Parameter(Mandatory = $true)][string]$Out
)

$ErrorActionPreference = 'Stop'

try {
  $sidType = [System.Security.Principal.SecurityIdentifier]
  $trusted = @(
    'S-1-5-32-544', # Administrators
    'S-1-5-18', # SYSTEM
    'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464' # TrustedInstaller
  )
  # CREATOR OWNER only stands for whoever creates something inside; that
  # needs a right to create there, which the entries below are about.
  $creatorOwner = 'S-1-3-0'
  # Any right that changes the folder or what is in it: write/add file,
  # append/add subfolder, write extended attributes, delete children, write
  # attributes, delete, change permissions, take ownership, generic all and
  # generic write.
  $writeMask = 0x2 -bor 0x4 -bor 0x10 -bor 0x40 -bor 0x100 -bor 0x10000 -bor 0x40000 -bor 0x80000 -bor
    0x10000000 -bor 0x40000000

  $acl = Get-Acl -LiteralPath $Path
  $found = @()
  $owner = $acl.GetOwner($sidType).Value
  if ($trusted -notcontains $owner) {
    $found += "owner $owner"
  }
  foreach ($rule in $acl.GetAccessRules($true, $true, $sidType)) {
    $who = $rule.IdentityReference.Value
    if ($rule.AccessControlType -ne 'Allow' -or $trusted -contains $who -or $who -eq $creatorOwner) {
      continue
    }
    if (([int64]$rule.FileSystemRights -band $writeMask) -ne 0) {
      $inherited = if ($rule.IsInherited) { 'inherited' } else { 'explicit' }
      $found += "$who may write ($($rule.FileSystemRights), $inherited)"
    }
  }
  if ($found.Count -eq 0) {
    $verdict = 'ok'
  } else {
    $verdict = 'fix: ' + ($found -join '; ')
  }
} catch {
  $verdict = 'error: ' + $_.Exception.Message
}

[System.IO.File]::WriteAllText($Out, $verdict)
