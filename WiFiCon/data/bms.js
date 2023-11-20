$(function () {
    $("#email").click(function(event) {
        $.ajax({
            type: "POST",
            url: "/email",
            success: function (data) {
                $("#savesuccess").show().delay(2000).fadeOut(500);
            },
            error: function (data) {
                $("#saveerror").show().delay(2000).fadeOut(500);
            }
        });
        event.preventDefault();
    });
    $("#menu > a").on('click',function (event) {
        $('#menu > a').removeClass("active");
        $(this).addClass("active");
        showContent($(this).attr("href"));
        getSettings($(this).attr("href"));
        event.preventDefault();
    });
    $(".error,.success").hide();
    $("#maxY,#minY").change(function () {
        config.options.scales.yAxes[0].ticks.max = parseFloat($("#maxY").val());
        config.options.scales.yAxes[0].ticks.min = parseFloat($("#minY").val());
        myChart.update();
    });
    $("#soc a").click(function(event) {
        $.ajax({
            type: "POST",
            url: "/fullChg",
            dataType:'json',
            success: function () {
                if (!$("#soc .v").hasClass("manoff"))
                    $("#soc .v").addClass("manoff");
                else $("#soc .v").removeClass("manoff");
                $("#savesuccess").show().delay(2000).fadeOut(500);
            },
            error: function (data) {
                $("#saveerror").show().delay(2000).fadeOut(500);
            }
        });
        return false;
    });
    $("#lastEventTime a").click(function(event) {
        $.ajax({
            type: "GET",
            url: "/hideLastEventTime",
            success: function (data) {
                $("#savesuccess").show().delay(2000).fadeOut(500);
            },
            error: function (data) {
                $("#saveerror").show().delay(2000).fadeOut(500);
            }
        });
        return false;
    });
    showContent("cells");
    initChart();
    Setup();
    queryBMS();
});

var RELAY_TOTAL=0;
var W_RELAY_TOTAL=0;
var nCells=-1;
var myChart,config;
var nonCellLines = 3;

function showContent(c) {
    $("#content > div").hide();
    $("#"+c).show();
}

function initChart() {
    config = {
        type: 'line',
        data: {
            labels: [],
            datasets: []
        },
        options: {
            responsive: true,
            legend: {
                display: false
            },
            tooltips: {
                mode: 'index',
                intersect: false,
            },
            hover: {
                mode: 'nearest',
                intersect: true
            },
            scales: {
                xAxes: [{
                    type: 'time',
                    time: {
                        parser: 'HH:mm:ss',
                        unit: 'second'
                    },
                    scaleLabel: {
                        display: true,
                        labelString: 'Date'
                    }
                }],
                yAxes: [{
                    display: true,
                    id: 'cv',
                    scaleLabel: {
                        display: true,
                        labelString: 'Cell Volts'
                    },
                    ticks: { max: 3.6, min: 3.1 }
                },{
                    display: true,
                    id: 'pv',
                    scaleLabel: {
                        display: true,
                        labelString: 'Volts'
                    },
                    ticks: { max: 28, min: 22 }
                },{
                    display: true,
                    id: 'pa',
                    scaleLabel: {
                        display: true,
                        labelString: 'Amps'
                    },
                    ticks: { max: 60, min: -200 }
                },{
                    display: true,
                    id: 'sa',
                    scaleLabel: {
                        display: true,
                        labelString: 'PV Amps'
                    },
                    ticks: { max: 60, min: 0 }
                }]
            }
        }
    };
    myChart = new Chart(document.getElementById('cellCan'), config);
}

var colors = [ '#e6194B', '#3cb44b', '#ffe119', '#4363d8', '#f58231', '#42d4f4', '#f032e6', '#fabed4', '#469990', '#dcbeff', '#9A6324', '#fffac8', '#800000', '#aaffc3', '#000075', '#a9a9a9' ];

function initCells() {
    config.data.labels = [];
    config.data.datasets = [];
    config.data.datasets[0] = {
        label: 'Pack V',
        backgroundColor: '#ffffff',
        borderColor: '#ffffff',
        yAxisID: 'pv',
        data: [ ],
        fill: false
    };
    config.data.datasets[1] = {
        label: 'Pack A',
        backgroundColor: '#000000',
        borderColor: '#000000',
        yAxisID: 'pa',
        data: [ ],
        fill: false
    };
    config.data.datasets[2] = {
        label: 'Solar A',
        backgroundColor: '#CCCCCC',
        borderColor: '#CCCCCC',
        yAxisID: 'sa',
        data: [ ],
        fill: false
    };
    for (var i=0;i<nCells;i++) {
        config.data.datasets[i+nonCellLines] = {
            label: 'Cell '+i,
            backgroundColor: colors[i],
            borderColor: colors[i],
            yAxisID: 'cv',
            data: [ ],
            fill: false
        }
    }
    myChart.update();
    $("[cellval]").remove();
    $("[celldel]").remove();
    for (var rel=0;rel<nCells;rel++) {
        var temp = $("#cellTV").clone();
        temp.attr({id: "cellTV"+rel});
        temp.find(".h").text('#'+rel+': T mV').css("background-color", colors[rel]);
        temp.attr({cellval: true});
        temp.insertBefore("#cellTV");
        var theA = temp.find("a");
        theA.attr("cell",rel);
        theA.click(function () {
            var r = $(this).attr("cell");
            var min = 0;
            if (!$("#cellTV"+r+" .v").hasClass("dumping"))
               min = (Number($('#DurH').val()) * 60) + Number($('#DurM').val());
            $.ajax({
                type: "POST",
                url: "/dump",
                data: { cell: r, min: min},
                dataType:'json',
                success: function (data) {
                    $("#savesuccess").show().delay(2000).fadeOut(500);
                },
                error: function (data) {
                    $("#saveerror").show().delay(2000).fadeOut(500);
                }
            });
            return false;
         });
        temp.show();

        temp = $("#cellDel").clone();
        temp.find("span[cell]").text('#'+rel).css("background-color", colors[rel]);
        temp.attr({id: "cellDel"+rel});
        temp.attr({celldel: true});
        var theA = temp.find("a[forget]");
        theA.attr("cell",rel);
        theA.click(function () {
            var r = $(this).attr("cell");
            $.ajax({
                type: "POST",
                url: "/forget",
                data: { cell: r },
                dataType:'json',
                success: function (data) {
                    $("#savesuccess").show().delay(2000).fadeOut(500);
                },
                error: function (data) {
                    $("#saveerror").show().delay(2000).fadeOut(500);
                }
            });
            return false;
         });
        var theI = temp.find("a[move]");
        theI.attr("cell",rel);
        theI.attr({id: "cellMove"+rel});
        theI.click(function() {
            var r = $(this).attr("cell");
            $.ajax({
                type: "POST",
                url: "/move",
                data: { cell: r },
                dataType:'json',
                success: function (data) {
                    $("#savesuccess").show().delay(2000).fadeOut(500);
                },
                error: function (data) {
                    $("#saveerror").show().delay(2000).fadeOut(500);
                }
            });
        });
        temp.insertBefore("#cellDel");
        temp.show();
    }
}

function getSettings(s) {
    $.getJSON(s,
        function (data) {
            if (data.notRecd) {
                $("#errmess").show().text("Settings not received");
                return;
            }
            $("#errmess").hide();
            if (s == "net") {
                $("input[name='apName']").val(data.apName);
                $("input[name='apPW']").val("");
                $("input[name='password']").val("");
                $("input[name='email']").val(data.email);
                $("input[name='senderEmail']").val(data.senderEmail);
                $("input[name='senderServer']").val(data.senderServer);
                $("input[name='senderPort']").val(data.senderPort);
                $("input[name='senderSubject']").val(data.senderSubject);
                $("input[name='logEmail']").val(data.logEmail);
                $("input[name='doLogging']").prop("checked", data.doLogging);
                $("input[name='ssid']").val(data.ssid);
            } else if (s == "cells") {
            } else if (s == "slides") {
                for (var i=0;i<W_RELAY_TOTAL;i++)
                    $("#slideControl"+i).hide();
                $.each(data.relaySettings, function (index, value) {
                    $("#slideName" + value.relay).text(value.name);
                    $("#slideControl"+value.relay).show();
                });
            } else if (s == "events") {
                $('#eventTable > tbody').empty();
                $.each(data.events, function (index, value) {
                    $('#eventTable > tbody').append($('<tr>')
                        .append($('<td>').append(value.cmd))
                        .append($('<td>').append(new Date(1000 * value.when).toLocaleString()))
                        .append($('<td>').append(value.cell))
                        .append($('<td>').append(value.tC))
                        .append($('<td>').append(value.mV))
                        .append($('<td>').append(value.amps))
                        .append($('<td>').append(value.relay))
                        .append($('<td>').append(value.xtra))
                        .append($('<td>').append(value.ms)));
                });
            } else if (s == "temps") {
                $("input[name='t1B']").val(data.t1B);
                $("input[name='t1R']").val(data.t1R);
                $("input[name='t2B']").val(data.t2B);
                $("input[name='t2R']").val(data.t2R);
                $("#nTSets").val(data.nTSets);
                setupTemps(data);
            } else if (s == "batt") {
                $("#BattAH").val(data.BattAH);
                $("#socLastAdj").html(data.socLastAdj);
                $("#BatAHMeasured").html(data.BatAHMeasured);
                $("#nCells").val(data.nCells);
                $("#CurSOC").val('');
                $("#TopAmps").val(data.TopAmps);
                $("input[name='cellCnt']").val(data.cellCnt);
                $("input[name='cellDelay']").val(data.cellDelay);
                $("input[name='resPwrOn']").prop("checked", data.resPwrOn);
                $("input[name='cellTime']").val(data.cellTime);
            } else if (s == "limits") {
                $("input[name='ShuntErrTime']").val(data.ShuntErrTime);
                $("input[name='MainID']").val(data.MainID);
                $("input[name='PVID']").val(data.PVID);
                $("input[name='InvID']").val(data.InvID);
                $("#ChargePct").val(data.ChargePct);
                $("#ChargePctRec").val(data.ChargePctRec);
                $("#FloatV").val(data.FloatV);
                $("#ChargeRate").val(data.ChargeRate);
                $("#CellsOutMin").val(data.CellsOutMin);
                $("#CellsOutMax").val(data.CellsOutMax);
                $("#CellsOutTime").val(data.CellsOutTime);
                $("#bdVolts").val(data.bdVolts);
                $.each(data.limitSettings, function (index, value) {
                    $("#" + index).val(value);
                });
                $("#useBoardTemp").prop("checked", data.useBoardTemp);
                $("#useCellC").prop("checked", data.useCellC);


            } else if (s == "relays") {
                $("#slideMS").val(data.slideMS);
                $.each(data.relaySettings, function (index, value) {
                    $("#relayName" + index).val(value.name);
                    $("#relayFrom" + index).val(value.from);
                    setRelayType.call($("#relayType" + index).val(value.type));
                    $("#relayTrip" + index).val(value.trip);
                    $("#relayRec" + index).val(value.rec);
                });
            }
        }).fail(function () { }
        );
    return true;
}

function formatNum(n,d) {
    n= +n.toFixed(d) || 0;
    return n.toLocaleString(undefined, { maximumFractionDigits: d, minimumFractionDigits: d });
}

function queryBMS() {
    var pollFreq = 2000;
    var histSize = 100;
    $.getJSON("status", function (data) {
        if (data.debugstr) $("#debugstr").show().html(data.debugstr);
        else $("#debugstr").hide();
        if (data.lastEventTime) {
            $("#lastEventTime").show();
            $("#lastEventTime a").html(new Date(1000 * data.lastEventTime).toLocaleString());
        } else $("#lastEventTime").hide();

        if (data.RELAY_TOTAL && !RELAY_TOTAL) {
            setupRelays(data.RELAY_TOTAL);
            RELAY_TOTAL = data.RELAY_TOTAL;
        }
        if (data.W_RELAY_TOTAL && !W_RELAY_TOTAL) {
            setupWRelays(data.W_RELAY_TOTAL);
            W_RELAY_TOTAL = data.W_RELAY_TOTAL;
        }
        for (var i=0;i<RELAY_TOTAL;i++) {
            var rn = data["relayName"+i];
            if (rn && rn.length) {
                $("#relayStatus"+i).show();
                $("#relayStatus"+i+" .h").html(rn);
                if (data["relaySlide"+i] == null) {
                    if (data["relayOff"+i] == "off")
                        $("#relayStatus"+i+" a.v").addClass("manoff");
                    else $("#relayStatus"+i+" a.v").removeClass("manoff");
                    $("#relayStatus"+i+" div.v").hide();
                    $("#relayStatus"+i+" a.v").html(data["relayStatus"+i]).show();
                } else {
                    var pct = data["relaySlide"+i];
                    $("#relayStatus"+i+" div.v").html(!pct?"In":pct==100?"Out":pct).show();
                    $("#relayStatus"+i+" a.v").hide();
                }
            } else $("#relayStatus"+i).hide();
        }
        if (data.watchDogHits) {
            $("#watchDogHits").show();
            $("#watchDogHits .v").html(data.watchDogHits);
        } else $("#watchDogHits").hide();

        var d = new Date(1000 * data.now);
        $("#timenow").html(d.toLocaleTimeString([], {
            hour: '2-digit',
            minute: '2-digit'
          }).replace(" AM","").replace(" PM",""));
        $("#datenow").html(d.toLocaleDateString([],{ day: 'numeric', month: 'numeric' }));

        $("#cellvolt").hide();
        if (data.maxCellVState || data.minCellVState)
            $("#cellvolt").show();
        val = "";
        if (data.maxCellVState)
            val = "over";
        if (data.minCellVState)
            val = val + " under";
        $("#cellvolt .v").html(val);

        var packvolts = Number(data.packvolts)/1000;
        var dval = formatNum(packvolts,2);
        if (data.maxPackVState || data.minPackVState)
            $("#packvolts .v").addClass("highlighted");
        else $("#packvolts .v").removeClass("highlighted");
        if (data.maxPackVState)
            dval = "over " + formatNum(packvolts,2);
        if (data.minPackVState)
            dval = "under " + formatNum(packvolts,2);
        $("#packvolts .v").html(dval);

        var pc = Number(data.packcurrent);
        var pvc = Number(data.pvcurrent);
        $("#loadcurrent .v").html(formatNum((pvc - pc) / 1000,2));
        pc = pc / 1000;
        pvc = pvc / 1000;
        $("#packcurrent .v").html(formatNum(pc,2));
        $("#pvcurrent .v").html(formatNum(pvc,2));
        if (data.invcurrent){
            $("#invcurrent").show();
            $("#invcurrent .v").html(formatNum(Number(data.invcurrent) / 1000,2));
        } else
            $("#invcurrent").hide();
        $("#soc .v").html(data.soc);
        $("#soc .v").removeClass("manoff");
        $("#soc .v").removeClass("fullChg");
        if (!data.socvalid)
            $("#soc .v").addClass("manoff");
        else if (data.fullChg)
            $("#soc .v").addClass("fullChg");

        $("#celltemp").hide();
        if (data.maxCellCState || data.minCellCState)
            $("#celltemp").show();
        var val = "";
        if (data.maxCellCState)
            val = "over";
        if (data.minCellCState)
            val = val + " under";
        $("#celltemp .v").html(val);

        val = data.BoardTemp;
        if (data.useBoardTemp && (data.maxPackCState || data.minPackCState))
            $("#BoardTemp .v").addClass("highlighted")
        else $("#BoardTemp .v").removeClass("highlighted")
        if (data.useBoardTemp && data.maxPackCState)
            val = "over " + val;
        if (data.useBoardTemp && data.minPackCState)
            val = "under " + val;
        $("#BoardTemp .v").html(val);

        val = data.Temp1;
        if (val == -300) $("#Temp1").hide();
        else $("#Temp1").show();
        $("#Temp1 .v").html(val);
        
        val = data.Temp2;
        if (val == -300) $("#Temp2").hide();
        else $("#Temp2").show();
        $("#Temp2 .v").html(val);

        val = data.Water;
        if (val == 200) $("#Water").hide();
        else $("#Water").show();
        $("#Water .v").html(val + '%');

        val = data.Gas;
        if (val == 200) $("#Gas").hide();
        else $("#Gas").show();
        $("#Gas .v").html(val + '%');

        $("#uptimec .v").html(data.uptimec);
        $("#uptimew .v").html(data.uptimew);

        $("#nocontroller").hide();

        if (data.nCells != nCells) {
            nCells = data.nCells;
            initCells();
        }
        var dt = new Date();
        config.data.labels.push(dt);
        var t = new Date(dt.getTime() - (pollFreq*histSize));
        while (config.data.labels.length && config.data.labels[0] < t) {
            config.data.labels.shift();
            for (var i=0;i<nonCellLines;i++)
                config.data.datasets[i].data.shift();
            $.each(data.cells, function (index, value) {
                config.data.datasets[value.c+nonCellLines].data.shift();
            });
        }
        config.data.datasets[0].data.push(packvolts);
        config.data.datasets[1].data.push(pc);
        config.data.datasets[2].data.push(pvc);
        $.each(data.cells, function (index, value) {
            var ds = config.data.datasets[value.c+nonCellLines];
            if (value.l) {
                ds.pointStyle = 'cross';
                $(".v,.t","#cellTV"+value.c).addClass("manoff");
            } else {
                ds.pointStyle = 'circle';
                $(".v,.t","#cellTV"+value.c).removeClass("manoff");
            }
            var cData = ds.data;
            var valV = value.v / 1000.0;
            cData.push(valV);
            var cell = $("#cellTV"+value.c+" .v");
            if (value.v > 0) {
                if (value.t > -101)
                    $("#cellTV"+value.c+" .t").html(value.t);
                else $("#cellTV"+value.c+" .t").html("");
                $("#cellDel"+value.c+" .t").html(value.bt);
                cell.text(formatNum(valV,2));
            } else cell.text('');
            var inp = $("#cellMove"+value.c);
            if (value.d) {
                cell.addClass("dumping");
                inp.addClass("dumping");
            } else {
                cell.removeClass("dumping");
                inp.removeClass("dumping");
            }
        });
        myChart.update();
        setTimeout(queryBMS, pollFreq);
    }).fail(function (a,b,c) {
        $("#nocontroller").show();
        setTimeout(queryBMS, pollFreq);
    });
}

function convertTemp(doC,t) {
    var t = $(t + " .v").html();
    if (doC) t = (t-32)*5/9;
    else t = t*9/5 + 32;
    $(t + " .v").html(Math.round(t));
}

function toggleTemp() {
    $.ajax({
        type: "GET",
        url: "/toggleTemp",
        success: function (data) {
            convertTemp(data.val,"#BoardTemp");
            $("#savesuccess").show().delay(2000).fadeOut(500);
        },
        error: function (data) {
            $("#saveerror").show().delay(2000).fadeOut(500);
        }
    });
    return false;
}

function setRelayType() {
    var r = $(this).attr("relay");
    var val = $(this).val();
    if (val == "CP" || val == "LP" || val == "H" || val == "B" || val == "A")
        $("#relayDoSoC"+r).show();
    else $("#relayDoSoC"+r).hide();
    if (val == "L" || val == "LP")
        $("#relayDoFrom"+r).show();
    else $("#relayDoFrom"+r).hide();
    if (val == "A") {
        $("label[for='relayTrip"+r+"']").html("On Hold Dur (100ms):");
        $("label[for='relayRec"+r+"']").html("Off Hold Dur (100ms):");
    } else {
        $("label[for='relayTrip"+r+"']").html("Trip:");
        $("label[for='relayRec"+r+"']").html("Rec:");
    }
}

function setHiddenTime() {
    var d = new Date("1/1/22 " + $(this).val());
    var m = (d.getHours() * 60) + d.getMinutes();
    if (Number.isNaN(m)) {
        $(this).addClass('dumping');
        return;
    }
    $(this).removeClass('dumping');
    $("#"+$(this).attr('id').replace('Str','')).val(m);
}

function setupTemps(data) {
    $(".thermLive").remove();
    for (var t=data.nTSets-1;t>=0;t--) {
        var temp = $("#thermTemp").clone();
        temp.find("tr").addClass("thermLive")
        $.each(data.relaySettings,function(ind,val) {
            temp.find('#tsetRelay').append($(new Option(val.name,val.relay)));
        });
        $.each(['Sens','Relay','Start','End','StartStr','EndStr','Trip','Rec','0_','1_','2_','3_','4_','5_','6_'],function (index,value) {
            temp.find('#tset'+value).attr({id: "tset"+value+t, name: "tset"+value+t});
        });
        temp.find("[for]").each(function(index) {
            var val=$(this).attr("for");
            $(this).attr({for: val+t});
        });
        temp.find("tr").insertAfter("#lastTempRow");
        $("#tsetStartStr"+t).blur(setHiddenTime);
        $("#tsetEndStr"+t).blur(setHiddenTime);
    }
    $.each(data.tSets,function (index,value) {
        for (i=0;i<7;i++) {
            $("#tset" + i + "_" + index).prop("checked",value.dows & (1 << i));
        }
        $.each(['Sens','Relay','Trip','Rec'],function (i,v) {
            $("#tset" + v + index).val(value[v]);
        });
        $.each(['Start','End'],function(i,v) {
            var h = Math.floor(value[v] / 60);
            var m = value[v] % 60;
            var pm = Math.floor(h / 12);
            var mstr = ""+m;
            if (m < 10) mstr = "0"+m;
            $("#tset" + v + "Str" + index).val(""+Math.floor(h%12)+":"+mstr+" "+(pm?"pm":"am"));
        });
    });
}

function setupRelays(rt) {
    for (var rel=0;rel<rt;rel++) {
        var temp = $("#relay").clone();
        temp.attr({id: "relay"+rel});
        temp.find("[for='relayName']").text("J"+rel+":");
        if (rel < 10)
            temp.find("#relayType option[value='S'],#relayType option[value='D'],#relayType option[value='T']").hide();
        else
            temp.find("#relayType option[value='A']").hide();
        $.each(['Name','DoSoC','Type','Trip','Rec','DoFrom','From'],function (index,value) {
            temp.find('#relay'+value).attr({id: "relay"+value+rel, name: "relay"+value+rel});
        });
        temp.find("[for]").each(function(index) {
            var val=$(this).attr("for");
            $(this).attr({for: val+rel});
        });
        var theTy = temp.find("#relayType" + rel);
        theTy.attr("relay",rel);
        theTy.change(setRelayType);
        temp.insertBefore("#relay");
        temp = $("#relayStatus").clone();
        temp.attr({id: "relayStatus"+rel});
        var theA = temp.find("a.v");
        theA.attr("relay",rel);
        theA.click(function () {
            var r = $(this).attr("relay");
            $.ajax({
                type: "POST",
                url: "/saveOff",
                data: { relay: r},
                dataType:'json',
                success: function () {
                    if (!$("#relayStatus"+r+" .v").hasClass("manoff"))
                        $("#relayStatus"+r+" .v").addClass("manoff");
                    else $("#relayStatus"+r+" .v").removeClass("manoff");
                    $("#savesuccess").show().delay(2000).fadeOut(500);
                },
                error: function (data) {
                    $("#saveerror").show().delay(2000).fadeOut(500);
                }
            });
            return false;
        });
        temp.insertBefore("#relayStatus");
    }
    $("#relay").hide();
    $("#relayStatus").hide();
}

function setupWRelays(rt) {
    for (var rel=0;rel<rt;rel++) {
        temp = $("#slideControl").clone();
        temp.attr({id: "slideControl" + rel});
        temp.find("label").attr({id: "slideName"+rel});
        var theB = temp.find("button");
        theB.attr("relay",rel);
        theB.click(function () {
            var r = $(this).attr("relay");
            $.ajax({
            type: "POST",
            url: "/slide",
            data: { relay: r, dir:$(this).text() == "Out"},
            dataType: 'json',
            success: function (data) {
            },
            fail: function (data) {
                $("#saveerror").show().delay(2000).fadeOut(500);
            }});
            return false;
        });
        temp.insertBefore("#slideControl");
    }
    $("#slideControl").hide();
}

function doSubmit(e) {
    e.preventDefault();
    $("#errmess").hide();
    $.ajax({
        type: $(this).attr('method'),
        url: $(this).attr('action'),
        data: $(this).serialize(),
        success: function (data) {
            $("#savesuccess").show().delay(2000).fadeOut(500);
            if (!data.success) {
                $("#errmess").show().text(data.errmess);
            }
        },
        error: function (data) {
            $("#saveerror").show().delay(2000).fadeOut(500);
        },
    });
}

function Setup() {

    $("#loading").show();

    $("#BoardTemp a").click(toggleTemp);
    var LIMITNames = [['Volts (mV):','Temp (C):'],['Cell','Pack'],['Max','Min'],['Trip:','Rec:']];
    for (var l0=0;l0<LIMITNames[0].length;l0++) {
        for (var l1=0;l1<LIMITNames[1].length;l1++) {
            for (var l2=0;l2<LIMITNames[2].length;l2++) {
                var d = $("<div class='limits'></div>");
                for (var l3=0;l3<LIMITNames[3].length;l3++) {
                    var id=l0+""+l1+""+l2+""+l3;
                    var temp = $("#limit").clone();
                    var lab = temp.find("label");
                    if (l3) {
                        lab.html(LIMITNames[3][l3]);
                        lab.addClass('rec');
                    } else {
                        lab.addClass('field');
                        lab.html(LIMITNames[2][l2] + ' ' + LIMITNames[1][l1] + ' ' + LIMITNames[0][l0]);
                    }
                    var inp=temp.find("input");
                    inp.attr("for",id);
                    inp.attr("id",id);
                    inp.attr("name",id);
                    d.append(temp.children());
                }
                d.insertBefore("#limit");
            }
        }
    }
    $("#limit").remove();
    
    $("form").unbind('submit').submit(doSubmit);
    $("#slideStop,#allOut,#allIn").click(function(event) {
        $.ajax({
            type: "GET",
            url: '/'+$(this).attr('id'),
            success: function (data) {
                $("#savesuccess").show().delay(2000).fadeOut(500);
            },
            error: function (data) {
                $("#saveerror").show().delay(2000).fadeOut(500);
            }
        });
        return false;
    });
}