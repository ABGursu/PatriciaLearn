# Benchmark test script for PatriciaLearn engine
param(
    [string]$Scenario = "default"
)

$enginePath = ".\patricia.exe"
$env:Path += ";C:\msys64\mingw64\bin"

# Create input file based on scenario
$inputFile = "bench_input_$Scenario.txt"

switch ($Scenario) {
    "default" {
        @"
setoption name Armageddon Mode value false
setoption name Armageddon Psychology value false
setoption name Fortress Detection value false
bench
quit
"@ | Out-File -FilePath $inputFile -Encoding ASCII
    }
    "armageddon_only" {
        @"
setoption name Armageddon Mode value true
setoption name Armageddon Psychology value false
setoption name Fortress Detection value false
bench
quit
"@ | Out-File -FilePath $inputFile -Encoding ASCII
    }
    "armageddon_psychology" {
        @"
setoption name Armageddon Mode value true
setoption name Armageddon Psychology value true
setoption name Fortress Detection value false
bench
quit
"@ | Out-File -FilePath $inputFile -Encoding ASCII
    }
    "fortress_only" {
        @"
setoption name Armageddon Mode value false
setoption name Armageddon Psychology value false
setoption name Fortress Detection value true
bench
quit
"@ | Out-File -FilePath $inputFile -Encoding ASCII
    }
    "armageddon_fortress" {
        @"
setoption name Armageddon Mode value true
setoption name Armageddon Psychology value false
setoption name Fortress Detection value true
bench
quit
"@ | Out-File -FilePath $inputFile -Encoding ASCII
    }
    "armageddon_psychology_fortress" {
        @"
setoption name Armageddon Mode value true
setoption name Armageddon Psychology value true
setoption name Fortress Detection value true
bench
quit
"@ | Out-File -FilePath $inputFile -Encoding ASCII
    }
    "fortress_psychology" {
        @"
setoption name Armageddon Mode value false
setoption name Armageddon Psychology value true
setoption name Fortress Detection value true
bench
quit
"@ | Out-File -FilePath $inputFile -Encoding ASCII
    }
}

Write-Host "=== Scenario: $Scenario ===" -ForegroundColor Green
Get-Content $inputFile | & $enginePath 2>&1 | Select-String -Pattern "Bench:|nodes|nps" | Select-Object -Last 1

# Cleanup
Remove-Item $inputFile -ErrorAction SilentlyContinue

