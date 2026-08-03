# Multiple benchmark test runs for PatriciaLearn engine
param(
    [string]$Scenario = "default",
    [int]$Runs = 3
)

$enginePath = ".\patricia.exe"
$env:Path += ";C:\msys64\mingw64\bin"

# Create input file based on scenario
$inputFile = "bench_input_$Scenario.txt"

switch ($Scenario) {
    "default" {
        @"
setoption name Armageddon value false
setoption name Armageddon MCTS value false
setoption name Fortress Detection value false
bench
quit
"@ | Out-File -FilePath $inputFile -Encoding ASCII
    }
    "armageddon_only" {
        @"
setoption name Armageddon value true
setoption name Armageddon MCTS value false
setoption name Fortress Detection value false
bench
quit
"@ | Out-File -FilePath $inputFile -Encoding ASCII
    }
    "armageddon_mcts" {
        @"
setoption name Armageddon value false
setoption name Armageddon MCTS value true
setoption name Fortress Detection value false
bench
quit
"@ | Out-File -FilePath $inputFile -Encoding ASCII
    }
    "fortress_only" {
        @"
setoption name Armageddon value false
setoption name Armageddon MCTS value false
setoption name Fortress Detection value true
bench
quit
"@ | Out-File -FilePath $inputFile -Encoding ASCII
    }
    "armageddon_fortress" {
        @"
setoption name Armageddon value true
setoption name Armageddon MCTS value false
setoption name Fortress Detection value true
bench
quit
"@ | Out-File -FilePath $inputFile -Encoding ASCII
    }
    "armageddon_mcts_fortress" {
        @"
setoption name Armageddon value true
setoption name Armageddon MCTS value true
setoption name Fortress Detection value true
bench
quit
"@ | Out-File -FilePath $inputFile -Encoding ASCII
    }
    "mcts_fortress" {
        @"
setoption name Armageddon value false
setoption name Armageddon MCTS value true
setoption name Fortress Detection value true
bench
quit
"@ | Out-File -FilePath $inputFile -Encoding ASCII
    }
    "mcts_experience" {
        @"
setoption name Armageddon value false
setoption name Armageddon MCTS value true
setoption name Fortress Detection value false
setoption name Experience Enabled value true
setoption name Experience Book value false
bench
quit
"@ | Out-File -FilePath $inputFile -Encoding ASCII
    }
}

$nodesList = @()
$npsList = @()

for ($i = 1; $i -le $Runs; $i++) {
    $output = Get-Content $inputFile | & $enginePath 2>&1
    $benchLine = $output | Select-String -Pattern "Bench:"
    if ($benchLine) {
        if ($benchLine -match "Bench: (\d+) nodes (\d+) nps") {
            $nodes = [int64]$matches[1]
            $nps = [int64]$matches[2]
            $nodesList += $nodes
            $npsList += $nps
            Write-Host "Run $i : Nodes=$nodes, NPS=$nps" -ForegroundColor Gray
        }
    }
    Start-Sleep -Milliseconds 100
}

# Calculate averages
$avgNodes = ($nodesList | Measure-Object -Average).Average
$avgNPS = ($npsList | Measure-Object -Average).Average

Write-Host "`nAverage: Nodes=$([math]::Round($avgNodes)), NPS=$([math]::Round($avgNPS))" -ForegroundColor Green

# Cleanup
Remove-Item $inputFile -ErrorAction SilentlyContinue

# Return values for parsing
Write-Output "Nodes=$([math]::Round($avgNodes))"
Write-Output "NPS=$([math]::Round($avgNPS))"
